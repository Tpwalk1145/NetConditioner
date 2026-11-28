#include "engine/process_filter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "core/str.h"
#include "core/time.h"

#define NC_FILTER_REFRESH_MS 2000u
#define NC_FILTER_ON_DEMAND_NS (100ull * 1000000ull)
#define NC_NEGATIVE_CACHE_CAP 8192u
#define NC_NEGATIVE_TTL_NS (2ull * 1000000000ull)
#define NC_NEGATIVE_MAX_PROBE 32u

enum {
    NEG_SLOT_EMPTY = 0,
    NEG_SLOT_OCCUPIED = 1,
    NEG_SLOT_TOMBSTONE = 2
};

typedef struct {
    uint8_t state;
    NcNegativeKey key;
    uint64_t expires_ns;
} NegativeSlot;

struct NcNegativeCache {
    NegativeSlot *slots;
    size_t cap;
    size_t max_probe;
};

static uint64_t negative_hash(const NcNegativeKey *key) {
    return nc_flow_key_hash(key->protocol, &key->local, key->has_remote ? &key->remote : NULL);
}

static bool negative_key_equal(const NcNegativeKey *a, const NcNegativeKey *b) {
    return a->protocol == b->protocol && a->has_remote == b->has_remote &&
           nc_flow_endpoint_equal(&a->local, &b->local) &&
           (!a->has_remote || nc_flow_endpoint_equal(&a->remote, &b->remote));
}

NcNegativeCache *nc_negative_cache_new(size_t capacity) {
    size_t cap = 16;
    NcNegativeCache *cache;
    while (cap < capacity && cap < (SIZE_MAX / 2)) {
        cap *= 2;
    }
    cache = (NcNegativeCache *)calloc(1, sizeof(NcNegativeCache));
    if (!cache) {
        return NULL;
    }
    cache->slots = (NegativeSlot *)calloc(cap, sizeof(NegativeSlot));
    if (!cache->slots) {
        free(cache);
        return NULL;
    }
    cache->cap = cap;
    cache->max_probe = cap < NC_NEGATIVE_MAX_PROBE ? cap : NC_NEGATIVE_MAX_PROBE;
    return cache;
}

void nc_negative_cache_free(NcNegativeCache *cache) {
    if (!cache) {
        return;
    }
    free(cache->slots);
    free(cache);
}

bool nc_negative_cache_contains(NcNegativeCache *cache, const NcNegativeKey *key, uint64_t now_ns) {
    size_t mask;
    size_t base;
    size_t step;
    if (!cache || !cache->slots || !key || cache->cap == 0) {
        return false;
    }
    mask = cache->cap - 1;
    base = (size_t)(negative_hash(key) & mask);
    for (step = 0; step < cache->max_probe; step++) {
        NegativeSlot *slot = &cache->slots[(base + step) & mask];
        if (slot->state == NEG_SLOT_EMPTY) {
            return false;
        }
        if (slot->state == NEG_SLOT_TOMBSTONE) {
            continue;
        }
        if (negative_key_equal(&slot->key, key)) {
            if (now_ns < slot->expires_ns) {
                return true;
            }
            slot->state = NEG_SLOT_TOMBSTONE;
            return false;
        }
    }
    return false;
}

void nc_negative_cache_insert(NcNegativeCache *cache, const NcNegativeKey *key,
                              uint64_t now_ns, uint64_t ttl_ns) {
    size_t mask;
    size_t base;
    size_t step;
    NegativeSlot *reusable = NULL;
    NegativeSlot *oldest = NULL;
    if (!cache || !cache->slots || !key || cache->cap == 0) {
        return;
    }
    mask = cache->cap - 1;
    base = (size_t)(negative_hash(key) & mask);
    for (step = 0; step < cache->max_probe; step++) {
        NegativeSlot *slot = &cache->slots[(base + step) & mask];
        if (slot->state == NEG_SLOT_EMPTY) {
            NegativeSlot *target = reusable ? reusable : slot;
            target->state = NEG_SLOT_OCCUPIED;
            target->key = *key;
            target->expires_ns = now_ns + ttl_ns;
            return;
        }
        if (slot->state == NEG_SLOT_TOMBSTONE) {
            if (!reusable) {
                reusable = slot;
            }
            continue;
        }
        if (negative_key_equal(&slot->key, key)) {
            slot->expires_ns = now_ns + ttl_ns;
            return;
        }
        if (now_ns >= slot->expires_ns) {
            slot->state = NEG_SLOT_TOMBSTONE;
            if (!reusable) {
                reusable = slot;
            }
            continue;
        }
        if (!oldest || slot->expires_ns < oldest->expires_ns) {
            oldest = slot;
        }
    }
    {
        NegativeSlot *target = reusable ? reusable : oldest;
        if (target) {
            target->state = NEG_SLOT_OCCUPIED;
            target->key = *key;
            target->expires_ns = now_ns + ttl_ns;
        }
    }
}

typedef struct {
    DWORD *items;
    size_t count;
    size_t cap;
} OwnerList;

typedef struct {
    bool used;
    NcFlowEndpoint key;
    OwnerList owners;
} EndpointSlot;

typedef struct {
    EndpointSlot *slots;
    size_t cap;
    size_t count;
} EndpointMap;

typedef struct {
    bool used;
    NcFlowEndpoint local;
    NcFlowEndpoint remote;
    OwnerList owners;
} FlowSlot;

typedef struct {
    FlowSlot *slots;
    size_t cap;
    size_t count;
} FlowMap;

typedef struct {
    EndpointMap tcp;
    FlowMap tcp_flows;
    EndpointMap udp;
    DWORD *pids;
    size_t pid_count;
} Snapshot;

typedef enum {
    MATCH_TARGET = 0,
    MATCH_NON_TARGET,
    MATCH_UNKNOWN
} MatchState;

struct NcProcessFlowFilter {
    NcStringList names;
    SRWLOCK snapshot_lock;
    Snapshot *snapshot;
    volatile LONG stop;
    CRITICAL_SECTION refresh_lock;
    volatile LONG64 last_refresh_done_ns;
    volatile LONG64 last_refresh_req_ns;
    CRITICAL_SECTION negative_lock;
    NcNegativeCache *negative;
    HANDLE stop_event;
    HANDLE refresh_event;
    HANDLE thread;
    DWORD thread_id;
};

static uint16_t swap_port(DWORD field) {
    uint16_t value = (uint16_t)(field & 0xFFFFu);
    return (uint16_t)((value >> 8) | (value << 8));
}

static uint32_t swap_address(DWORD value) {
    return _byteswap_ulong((unsigned long)value);
}

static void endpoint_from_v4(NcFlowEndpoint *endpoint, DWORD address, DWORD port) {
    endpoint->family = 4;
    endpoint->address_high = 0;
    endpoint->address_low = swap_address(address);
    endpoint->port = swap_port(port);
}

static void endpoint_from_v6(NcFlowEndpoint *endpoint, const unsigned char *bytes, DWORD port) {
    uint64_t high = 0;
    uint64_t low = 0;
    size_t index;
    for (index = 0; index < 8; index++) {
        high = (high << 8) | bytes[index];
        low = (low << 8) | bytes[8 + index];
    }
    if (high == 0 && (low >> 32) == 0xFFFFull) {
        endpoint->family = 4;
        endpoint->address_high = 0;
        endpoint->address_low = low & 0xFFFFFFFFull;
    } else {
        endpoint->family = 6;
        endpoint->address_high = high;
        endpoint->address_low = low;
    }
    endpoint->port = swap_port(port);
}

static bool owners_add(OwnerList *owners, DWORD pid) {
    size_t index;
    if (!owners) {
        return false;
    }
    for (index = 0; index < owners->count; index++) {
        if (owners->items[index] == pid) {
            return true;
        }
    }
    if (owners->count == owners->cap) {
        size_t cap = owners->cap ? owners->cap * 2 : 4;
        DWORD *items = (DWORD *)realloc(owners->items, cap * sizeof(DWORD));
        if (!items) {
            return false;
        }
        owners->items = items;
        owners->cap = cap;
    }
    owners->items[owners->count++] = pid;
    return true;
}

static void owners_free(OwnerList *owners) {
    if (!owners) {
        return;
    }
    free(owners->items);
    owners->items = NULL;
    owners->count = 0;
    owners->cap = 0;
}

static bool pid_set_contains(const DWORD *pids, size_t count, DWORD pid) {
    size_t index;
    for (index = 0; index < count; index++) {
        if (pids[index] == pid) {
            return true;
        }
    }
    return false;
}

static MatchState owners_match(const OwnerList *owners, const DWORD *pids, size_t pid_count,
                               bool fail_open_ambiguous) {
    size_t index;
    if (!owners || owners->count == 0) {
        return MATCH_UNKNOWN;
    }
    if (fail_open_ambiguous && owners->count > 1) {
        return MATCH_NON_TARGET;
    }
    for (index = 0; index < owners->count; index++) {
        if (pid_set_contains(pids, pid_count, owners->items[index])) {
            return MATCH_TARGET;
        }
    }
    return MATCH_NON_TARGET;
}

static EndpointSlot *endpoint_map_find_slot(EndpointMap *map, const NcFlowEndpoint *key) {
    size_t mask;
    size_t index;
    if (!map || map->cap == 0) {
        return NULL;
    }
    mask = map->cap - 1;
    index = (size_t)(nc_flow_endpoint_hash(key) & mask);
    for (;;) {
        EndpointSlot *slot = &map->slots[index];
        if (!slot->used) {
            return slot;
        }
        if (nc_flow_endpoint_equal(&slot->key, key)) {
            return slot;
        }
        index = (index + 1) & mask;
    }
}

static bool endpoint_map_grow(EndpointMap *map) {
    size_t new_cap = map->cap ? map->cap * 2 : 64;
    EndpointSlot *old_slots = map->slots;
    size_t old_cap = map->cap;
    size_t index;
    EndpointSlot *new_slots = (EndpointSlot *)calloc(new_cap, sizeof(EndpointSlot));
    if (!new_slots) {
        return false;
    }
    map->slots = new_slots;
    map->cap = new_cap;
    map->count = 0;
    for (index = 0; index < old_cap; index++) {
        if (old_slots[index].used) {
            EndpointSlot *slot = endpoint_map_find_slot(map, &old_slots[index].key);
            *slot = old_slots[index];
            map->count++;
        }
    }
    free(old_slots);
    return true;
}

static bool endpoint_map_insert(EndpointMap *map, const NcFlowEndpoint *key, DWORD pid) {
    EndpointSlot *slot;
    if (!map || !key) {
        return false;
    }
    if (map->cap == 0 || (map->count + 1) * 10 > map->cap * 7) {
        if (!endpoint_map_grow(map)) {
            return false;
        }
    }
    slot = endpoint_map_find_slot(map, key);
    if (!slot) {
        return false;
    }
    if (!slot->used) {
        slot->used = true;
        slot->key = *key;
        memset(&slot->owners, 0, sizeof(slot->owners));
        map->count++;
    }
    return owners_add(&slot->owners, pid);
}

static const OwnerList *endpoint_map_lookup(const EndpointMap *map, const NcFlowEndpoint *key) {
    size_t mask;
    size_t index;
    if (!map || map->cap == 0 || !key) {
        return NULL;
    }
    mask = map->cap - 1;
    index = (size_t)(nc_flow_endpoint_hash(key) & mask);
    for (;;) {
        const EndpointSlot *slot = &map->slots[index];
        if (!slot->used) {
            return NULL;
        }
        if (nc_flow_endpoint_equal(&slot->key, key)) {
            return &slot->owners;
        }
        index = (index + 1) & mask;
    }
}

static FlowSlot *flow_map_find_slot(FlowMap *map, const NcFlowEndpoint *local,
                                    const NcFlowEndpoint *remote) {
    size_t mask;
    size_t index;
    if (!map || map->cap == 0) {
        return NULL;
    }
    mask = map->cap - 1;
    index = (size_t)(nc_flow_key_hash(6, local, remote) & mask);
    for (;;) {
        FlowSlot *slot = &map->slots[index];
        if (!slot->used) {
            return slot;
        }
        if (nc_flow_endpoint_equal(&slot->local, local) &&
            nc_flow_endpoint_equal(&slot->remote, remote)) {
            return slot;
        }
        index = (index + 1) & mask;
    }
}

static bool flow_map_grow(FlowMap *map) {
    size_t new_cap = map->cap ? map->cap * 2 : 64;
    FlowSlot *old_slots = map->slots;
    size_t old_cap = map->cap;
    size_t index;
    FlowSlot *new_slots = (FlowSlot *)calloc(new_cap, sizeof(FlowSlot));
    if (!new_slots) {
        return false;
    }
    map->slots = new_slots;
    map->cap = new_cap;
    map->count = 0;
    for (index = 0; index < old_cap; index++) {
        if (old_slots[index].used) {
            FlowSlot *slot = flow_map_find_slot(map, &old_slots[index].local, &old_slots[index].remote);
            *slot = old_slots[index];
            map->count++;
        }
    }
    free(old_slots);
    return true;
}

static bool flow_map_insert(FlowMap *map, const NcFlowEndpoint *local, const NcFlowEndpoint *remote,
                            DWORD pid) {
    FlowSlot *slot;
    if (!map || !local || !remote) {
        return false;
    }
    if (map->cap == 0 || (map->count + 1) * 10 > map->cap * 7) {
        if (!flow_map_grow(map)) {
            return false;
        }
    }
    slot = flow_map_find_slot(map, local, remote);
    if (!slot) {
        return false;
    }
    if (!slot->used) {
        slot->used = true;
        slot->local = *local;
        slot->remote = *remote;
        memset(&slot->owners, 0, sizeof(slot->owners));
        map->count++;
    }
    return owners_add(&slot->owners, pid);
}

static const OwnerList *flow_map_lookup(const FlowMap *map, const NcFlowEndpoint *local,
                                        const NcFlowEndpoint *remote) {
    size_t mask;
    size_t index;
    if (!map || map->cap == 0 || !local || !remote) {
        return NULL;
    }
    mask = map->cap - 1;
    index = (size_t)(nc_flow_key_hash(6, local, remote) & mask);
    for (;;) {
        const FlowSlot *slot = &map->slots[index];
        if (!slot->used) {
            return NULL;
        }
        if (nc_flow_endpoint_equal(&slot->local, local) &&
            nc_flow_endpoint_equal(&slot->remote, remote)) {
            return &slot->owners;
        }
        index = (index + 1) & mask;
    }
}

static Snapshot *snapshot_create(void) {
    return (Snapshot *)calloc(1, sizeof(Snapshot));
}

static void snapshot_free(Snapshot *snapshot) {
    size_t index;
    if (!snapshot) {
        return;
    }
    for (index = 0; index < snapshot->tcp.cap; index++) {
        if (snapshot->tcp.slots[index].used) {
            owners_free(&snapshot->tcp.slots[index].owners);
        }
    }
    free(snapshot->tcp.slots);
    for (index = 0; index < snapshot->tcp_flows.cap; index++) {
        if (snapshot->tcp_flows.slots[index].used) {
            owners_free(&snapshot->tcp_flows.slots[index].owners);
        }
    }
    free(snapshot->tcp_flows.slots);
    for (index = 0; index < snapshot->udp.cap; index++) {
        if (snapshot->udp.slots[index].used) {
            owners_free(&snapshot->udp.slots[index].owners);
        }
    }
    free(snapshot->udp.slots);
    free(snapshot->pids);
    free(snapshot);
}

static MatchState snapshot_lookup(const Snapshot *snapshot, uint8_t protocol,
                                  const NcFlowEndpoint *local, const NcFlowEndpoint *remote) {
    const OwnerList *owners;
    if (!snapshot || snapshot->pid_count == 0) {
        return MATCH_NON_TARGET;
    }
    if (protocol == 6) {
        if (remote) {
            owners = flow_map_lookup(&snapshot->tcp_flows, local, remote);
            if (owners) {
                return owners_match(owners, snapshot->pids, snapshot->pid_count, false);
            }
        }
        owners = endpoint_map_lookup(&snapshot->tcp, local);
        if (owners) {
            return owners_match(owners, snapshot->pids, snapshot->pid_count, false);
        }
        return MATCH_UNKNOWN;
    }
    if (protocol == 17) {
        owners = endpoint_map_lookup(&snapshot->udp, local);
        if (owners) {
            return owners_match(owners, snapshot->pids, snapshot->pid_count, true);
        }
        return MATCH_UNKNOWN;
    }
    return MATCH_NON_TARGET;
}

typedef void (*ProcessCallback)(DWORD pid, const wchar_t *image_name, void *context);

static void for_each_process(ProcessCallback callback, void *context) {
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    if (!callback) {
        return;
    }
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE || !snapshot) {
        return;
    }
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            callback(entry.th32ProcessID, entry.szExeFile, context);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

static bool process_path(DWORD pid, char **out) {
    HANDLE process;
    wchar_t buffer[1024];
    DWORD size = (DWORD)(sizeof(buffer) / sizeof(buffer[0]));
    *out = NULL;
    if (pid == 0) {
        return false;
    }
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }
    if (!QueryFullProcessImageNameW(process, 0, buffer, &size) || size == 0) {
        CloseHandle(process);
        return false;
    }
    CloseHandle(process);
    *out = nc_utf8_dup(buffer);
    return *out != NULL;
}

typedef struct {
    NcProcessList *list;
} EnumerateContext;

static void enumerate_callback(DWORD pid, const wchar_t *image_name, void *context) {
    EnumerateContext *enumerate = (EnumerateContext *)context;
    char *utf8 = nc_utf8_dup(image_name);
    Str display;
    NcProcessInfo info;
    if (!utf8) {
        return;
    }
    str_init(&display);
    nc_normalize_display_name(utf8, &display);
    free(utf8);
    if (display.len == 0) {
        str_free(&display);
        return;
    }
    info.name = nc_strdup(str_cstr(&display));
    str_free(&display);
    if (!info.name) {
        return;
    }
    info.path = NULL;
    process_path(pid, &info.path);
    if (enumerate->list->count == enumerate->list->cap) {
        size_t cap = enumerate->list->cap ? enumerate->list->cap * 2 : 64;
        NcProcessInfo *items = (NcProcessInfo *)realloc(enumerate->list->items, cap * sizeof(NcProcessInfo));
        if (!items) {
            free(info.name);
            free(info.path);
            return;
        }
        enumerate->list->items = items;
        enumerate->list->cap = cap;
    }
    enumerate->list->items[enumerate->list->count++] = info;
}

bool nc_process_enumerate(NcProcessList *list) {
    EnumerateContext context;
    if (!list) {
        return false;
    }
    memset(list, 0, sizeof(*list));
    context.list = list;
    for_each_process(enumerate_callback, &context);
    return true;
}

void nc_process_list_free(NcProcessList *list) {
    size_t index;
    if (!list) {
        return;
    }
    for (index = 0; index < list->count; index++) {
        free(list->items[index].name);
        free(list->items[index].path);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

typedef struct {
    NcProcessFlowFilter *filter;
    DWORD *pids;
    size_t count;
    size_t cap;
} ResolveContext;

static void resolve_callback(DWORD pid, const wchar_t *image_name, void *context) {
    ResolveContext *resolve = (ResolveContext *)context;
    char *utf8 = nc_utf8_dup(image_name);
    Str normalized;
    size_t index;
    bool match = false;
    if (!utf8) {
        return;
    }
    str_init(&normalized);
    nc_normalize_process_name(utf8, &normalized);
    free(utf8);
    if (normalized.len == 0) {
        str_free(&normalized);
        return;
    }
    for (index = 0; index < resolve->filter->names.count; index++) {
        if (strcmp(resolve->filter->names.items[index], str_cstr(&normalized)) == 0) {
            match = true;
            break;
        }
    }
    str_free(&normalized);
    if (!match) {
        return;
    }
    if (resolve->count == resolve->cap) {
        size_t cap = resolve->cap ? resolve->cap * 2 : 16;
        DWORD *pids = (DWORD *)realloc(resolve->pids, cap * sizeof(DWORD));
        if (!pids) {
            return;
        }
        resolve->pids = pids;
        resolve->cap = cap;
    }
    if (!pid_set_contains(resolve->pids, resolve->count, pid)) {
        resolve->pids[resolve->count++] = pid;
    }
}

static bool read_dynamic_table(void **out, DWORD *out_size,
                               DWORD (*query)(PVOID, PDWORD, BOOL, ULONG, ULONG, ULONG),
                               ULONG family, ULONG table_class) {
    int attempt;
    for (attempt = 0; attempt < 4; attempt++) {
        DWORD size = 0;
        DWORD result = query(NULL, &size, FALSE, family, table_class, 0);
        void *buffer;
        if (!(result == 0u || result == (DWORD)ERROR_INSUFFICIENT_BUFFER) || size == 0) {
            return false;
        }
        buffer = malloc(size);
        if (!buffer) {
            return false;
        }
        result = query(buffer, &size, FALSE, family, table_class, 0);
        if (result == 0) {
            *out = buffer;
            *out_size = size;
            return true;
        }
        free(buffer);
        if (result != (DWORD)ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
    }
    return false;
}

static bool read_tcp_table(Snapshot *snapshot, ULONG family) {
    void *buffer = NULL;
    DWORD size = 0;
    DWORD count;
    if (!read_dynamic_table(&buffer, &size, (DWORD (*)(PVOID, PDWORD, BOOL, ULONG, ULONG, ULONG))GetExtendedTcpTable,
                            family, TCP_TABLE_OWNER_PID_ALL)) {
        return false;
    }
    if (size < 4) {
        free(buffer);
        return false;
    }
    memcpy(&count, buffer, 4);
    if (family == (ULONG)AF_INET) {
        size_t row_size = sizeof(MIB_TCPROW_OWNER_PID);
        size_t index;
        for (index = 0; index < count; index++) {
            size_t offset = 4 + index * row_size;
            MIB_TCPROW_OWNER_PID row;
            NcFlowEndpoint local;
            NcFlowEndpoint remote;
            if (offset + row_size > size) {
                free(buffer);
                return false;
            }
            memcpy(&row, (unsigned char *)buffer + offset, row_size);
            endpoint_from_v4(&local, row.dwLocalAddr, row.dwLocalPort);
            if (local.port == 0) {
                continue;
            }
            endpoint_from_v4(&remote, row.dwRemoteAddr, row.dwRemotePort);
            endpoint_map_insert(&snapshot->tcp, &local, row.dwOwningPid);
            flow_map_insert(&snapshot->tcp_flows, &local, &remote, row.dwOwningPid);
        }
    } else {
        size_t row_size = sizeof(MIB_TCP6ROW_OWNER_PID);
        size_t index;
        for (index = 0; index < count; index++) {
            size_t offset = 4 + index * row_size;
            MIB_TCP6ROW_OWNER_PID row;
            NcFlowEndpoint local;
            NcFlowEndpoint remote;
            if (offset + row_size > size) {
                free(buffer);
                return false;
            }
            memcpy(&row, (unsigned char *)buffer + offset, row_size);
            endpoint_from_v6(&local, row.ucLocalAddr, row.dwLocalPort);
            if (local.port == 0) {
                continue;
            }
            endpoint_from_v6(&remote, row.ucRemoteAddr, row.dwRemotePort);
            endpoint_map_insert(&snapshot->tcp, &local, row.dwOwningPid);
            flow_map_insert(&snapshot->tcp_flows, &local, &remote, row.dwOwningPid);
        }
    }
    free(buffer);
    return true;
}

static bool read_udp_table(Snapshot *snapshot, ULONG family) {
    void *buffer = NULL;
    DWORD size = 0;
    DWORD count;
    if (!read_dynamic_table(&buffer, &size, (DWORD (*)(PVOID, PDWORD, BOOL, ULONG, ULONG, ULONG))GetExtendedUdpTable,
                            family, UDP_TABLE_OWNER_PID)) {
        return false;
    }
    if (size < 4) {
        free(buffer);
        return false;
    }
    memcpy(&count, buffer, 4);
    if (family == (ULONG)AF_INET) {
        size_t row_size = sizeof(MIB_UDPROW_OWNER_PID);
        size_t index;
        for (index = 0; index < count; index++) {
            size_t offset = 4 + index * row_size;
            MIB_UDPROW_OWNER_PID row;
            NcFlowEndpoint endpoint;
            if (offset + row_size > size) {
                free(buffer);
                return false;
            }
            memcpy(&row, (unsigned char *)buffer + offset, row_size);
            endpoint_from_v4(&endpoint, row.dwLocalAddr, row.dwLocalPort);
            if (endpoint.port != 0) {
                endpoint_map_insert(&snapshot->udp, &endpoint, row.dwOwningPid);
            }
        }
    } else {
        size_t row_size = sizeof(MIB_UDP6ROW_OWNER_PID);
        size_t index;
        for (index = 0; index < count; index++) {
            size_t offset = 4 + index * row_size;
            MIB_UDP6ROW_OWNER_PID row;
            NcFlowEndpoint endpoint;
            if (offset + row_size > size) {
                free(buffer);
                return false;
            }
            memcpy(&row, (unsigned char *)buffer + offset, row_size);
            endpoint_from_v6(&endpoint, row.ucLocalAddr, row.dwLocalPort);
            if (endpoint.port != 0) {
                endpoint_map_insert(&snapshot->udp, &endpoint, row.dwOwningPid);
            }
        }
    }
    free(buffer);
    return true;
}

static Snapshot *snapshot_build(NcProcessFlowFilter *filter) {
    ResolveContext resolve;
    Snapshot *snapshot;
    memset(&resolve, 0, sizeof(resolve));
    resolve.filter = filter;
    for_each_process(resolve_callback, &resolve);
    snapshot = snapshot_create();
    if (!snapshot) {
        free(resolve.pids);
        return NULL;
    }
    snapshot->pids = resolve.pids;
    snapshot->pid_count = resolve.count;
    if (!read_tcp_table(snapshot, AF_INET) || !read_tcp_table(snapshot, AF_INET6) ||
        !read_udp_table(snapshot, AF_INET) || !read_udp_table(snapshot, AF_INET6)) {
        snapshot_free(snapshot);
        return NULL;
    }
    return snapshot;
}

static void snapshot_publish(NcProcessFlowFilter *filter, Snapshot *snapshot) {
    Snapshot *old;
    AcquireSRWLockExclusive(&filter->snapshot_lock);
    old = filter->snapshot;
    filter->snapshot = snapshot;
    ReleaseSRWLockExclusive(&filter->snapshot_lock);
    snapshot_free(old);
}

static MatchState current_lookup(NcProcessFlowFilter *filter, uint8_t protocol,
                                 const NcFlowEndpoint *local, const NcFlowEndpoint *remote) {
    MatchState state;
    AcquireSRWLockShared(&filter->snapshot_lock);
    state = snapshot_lookup(filter->snapshot, protocol, local, remote);
    ReleaseSRWLockShared(&filter->snapshot_lock);
    return state;
}

bool nc_process_filter_refresh_now(NcProcessFlowFilter *filter) {
    Snapshot *snapshot;
    if (!filter || InterlockedCompareExchange(&filter->stop, 0, 0) != 0) {
        return false;
    }
    EnterCriticalSection(&filter->refresh_lock);
    if (InterlockedCompareExchange(&filter->stop, 0, 0) != 0) {
        LeaveCriticalSection(&filter->refresh_lock);
        return false;
    }
    snapshot = snapshot_build(filter);
    InterlockedExchange64(&filter->last_refresh_done_ns, (LONG64)nc_monotonic_ns());
    LeaveCriticalSection(&filter->refresh_lock);
    if (!snapshot) {
        return false;
    }
    snapshot_publish(filter, snapshot);
    return true;
}

/* Non-blocking async refresh trigger for packet hot path. */
static bool request_async_refresh(NcProcessFlowFilter *filter, uint64_t now) {
    LONG64 prev = InterlockedCompareExchange64(&filter->last_refresh_req_ns, 0, 0);
    if (prev > 0 && now >= (uint64_t)prev && (now - (uint64_t)prev) < NC_FILTER_ON_DEMAND_NS) {
        return false;
    }
    if (InterlockedCompareExchange64(&filter->last_refresh_req_ns, (LONG64)now, prev) != prev) {
        return false;
    }
    if (filter->refresh_event) {
        SetEvent(filter->refresh_event);
    }
    return true;
}

static unsigned __stdcall filter_thread_main(void *parameter) {
    NcProcessFlowFilter *filter = (NcProcessFlowFilter *)parameter;
    HANDLE waits[2];
    waits[0] = filter->stop_event;
    waits[1] = filter->refresh_event;
    for (;;) {
        DWORD wait;
        if (InterlockedCompareExchange(&filter->stop, 0, 0) != 0) {
            break;
        }
        wait = WaitForMultipleObjects(2, waits, FALSE, NC_FILTER_REFRESH_MS);
        if (wait == WAIT_OBJECT_0 || InterlockedCompareExchange(&filter->stop, 0, 0) != 0) {
            break;
        }
        nc_process_filter_refresh_now(filter);
    }
    return 0;
}

NcProcessFlowFilter *nc_process_filter_new(const char *const *names, size_t count) {
    NcProcessFlowFilter *filter;
    size_t index;
    filter = (NcProcessFlowFilter *)calloc(1, sizeof(NcProcessFlowFilter));
    if (!filter) {
        return NULL;
    }
    nc_string_list_init(&filter->names);
    for (index = 0; names && index < count; index++) {
        Str normalized;
        str_init(&normalized);
        nc_normalize_process_name(names[index], &normalized);
        if (normalized.len > 0) {
            nc_string_list_push(&filter->names, str_cstr(&normalized));
        }
        str_free(&normalized);
    }
    InitializeSRWLock(&filter->snapshot_lock);
    InitializeCriticalSection(&filter->refresh_lock);
    InitializeCriticalSection(&filter->negative_lock);
    filter->snapshot = snapshot_create();
    filter->negative = nc_negative_cache_new(NC_NEGATIVE_CACHE_CAP);
    filter->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    filter->refresh_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (filter->names.count == 0 || !filter->snapshot || !filter->negative ||
        !filter->stop_event || !filter->refresh_event) {
        return filter;
    }
    nc_process_filter_refresh_now(filter);
    {
        unsigned tid = 0;
        uintptr_t thread = _beginthreadex(NULL, 0, filter_thread_main, filter, 0, &tid);
        filter->thread = (HANDLE)thread;
        filter->thread_id = (DWORD)tid;
    }
    return filter;
}

void nc_process_filter_shutdown(NcProcessFlowFilter *filter) {
    HANDLE thread;
    if (!filter) {
        return;
    }
    InterlockedExchange(&filter->stop, 1);
    if (filter->stop_event) {
        SetEvent(filter->stop_event);
    }
    thread = filter->thread;
    filter->thread = NULL;
    if (thread) {
        if (filter->thread_id != GetCurrentThreadId()) {
            WaitForSingleObject(thread, INFINITE);
        }
        CloseHandle(thread);
    }
}

void nc_process_filter_free(NcProcessFlowFilter *filter) {
    if (!filter) {
        return;
    }
    nc_process_filter_shutdown(filter);
    if (filter->stop_event) {
        CloseHandle(filter->stop_event);
    }
    if (filter->refresh_event) {
        CloseHandle(filter->refresh_event);
    }
    nc_string_list_free(&filter->names);
    snapshot_free(filter->snapshot);
    DeleteCriticalSection(&filter->refresh_lock);
    DeleteCriticalSection(&filter->negative_lock);
    nc_negative_cache_free(filter->negative);
    free(filter);
}

bool nc_process_filter_match(NcProcessFlowFilter *filter, uint8_t protocol,
                             const NcFlowEndpoint *local, const NcFlowEndpoint *remote) {
    MatchState state;
    NcNegativeKey key;
    uint64_t now;
    bool cached;
    if (!filter || filter->names.count == 0) {
        return true;
    }
    state = current_lookup(filter, protocol, local, remote);
    if (state == MATCH_TARGET) {
        return true;
    }
    if (state == MATCH_NON_TARGET) {
        return false;
    }
    memset(&key, 0, sizeof(key));
    key.protocol = protocol;
    key.local = *local;
    key.has_remote = remote != NULL;
    if (remote) {
        key.remote = *remote;
    }
    now = nc_monotonic_ns();
    EnterCriticalSection(&filter->negative_lock);
    cached = nc_negative_cache_contains(filter->negative, &key, now);
    LeaveCriticalSection(&filter->negative_lock);
    if (cached) {
        return false;
    }
    if (!request_async_refresh(filter, now)) {
        LONG64 done = InterlockedCompareExchange64(&filter->last_refresh_done_ns, 0, 0);
        if (done > 0 && now >= (uint64_t)done && (now - (uint64_t)done) < NC_FILTER_ON_DEMAND_NS) {
            EnterCriticalSection(&filter->negative_lock);
            nc_negative_cache_insert(filter->negative, &key, now, NC_NEGATIVE_TTL_NS);
            LeaveCriticalSection(&filter->negative_lock);
        }
    }
    return false;
}
