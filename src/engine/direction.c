#include "direction.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windivert.h"

#define NC_MAX_QUEUE_PACKETS 4096u
#define NC_MAX_QUEUE_BYTES (8u * 1024u * 1024u)
#define NC_POOL_LIMIT 1024u
#define NC_ERROR_NO_DATA 232
#define NC_WAIT_SLICE_MS 100u

typedef struct QueuedItem {
    unsigned char *data;
    size_t cap;
    size_t length;
    unsigned char address[NC_WINDIVERT_ADDRESS_SIZE];
    uint64_t due_ns;
    uint64_t expires_ns;
    uint64_t sequence;
} QueuedItem;

typedef struct {
    QueuedItem **items;
    size_t len;
    size_t cap;
    size_t bytes;
} PacketQueue;

typedef struct {
    QueuedItem **items;
    size_t len;
    size_t cap;
} PacketPool;

struct NcDirectionEngine {
    char display[32];
    char *filter;
    bool outbound;
    NcDirectionProfile profile;
    NcConditionProtocol protocol;
    NcProcessFlowFilter *process_filter;
    NcEngineEvents *events;
    NcDirectionStats stats;
    uint64_t max_queue_ns;
    bool needs_queue;
    volatile LONG64 windivert_handle;
    CRITICAL_SECTION queue_lock;
    CONDITION_VARIABLE queue_cv;
    PacketQueue queue;
    CRITICAL_SECTION pool_lock;
    PacketPool pool;
    volatile LONG stop;
    volatile LONG receiver_done;
    volatile LONG fault_reported;
    HANDLE receiver_thread;
    HANDLE sender_thread;
    bool started;
    bool start_attempted;
    uint64_t base_ns;
};

static uint64_t sat_add(uint64_t value, uint64_t delta) {
    return value > UINT64_MAX - delta ? UINT64_MAX : value + delta;
}

void nc_burst_loss_init(NcBurstLossState *state, int32_t allow_ms, int32_t drop_ms, uint64_t now_ns) {
    if (!state) {
        return;
    }
    state->allow_ns = allow_ms > 0 ? (uint64_t)allow_ms * 1000000ull : 0;
    state->drop_ns = drop_ms > 0 ? (uint64_t)drop_ms * 1000000ull : 0;
    state->phase_start_ns = now_ns;
}

bool nc_burst_loss_should_drop(const NcBurstLossState *state, uint64_t now_ns) {
    uint64_t cycle;
    uint64_t elapsed;
    if (!state) {
        return false;
    }
    if (state->allow_ns == 0 && state->drop_ns == 0) {
        return false;
    }
    if (state->allow_ns == 0) {
        return true;
    }
    if (state->drop_ns == 0) {
        return false;
    }
    cycle = sat_add(state->allow_ns, state->drop_ns);
    elapsed = now_ns > state->phase_start_ns ? now_ns - state->phase_start_ns : 0;
    return (elapsed % cycle) >= state->allow_ns;
}

uint64_t nc_packet_service_ns(size_t length, uint64_t bandwidth_kbps) {
    uint64_t service;
    if (bandwidth_kbps == 0 || length == 0) {
        return 0;
    }
    service = ((uint64_t)length * 8ull * 1000000000ull) / (bandwidth_kbps * 1000ull);
    return service == 0 ? 1 : service;
}

void nc_bandwidth_scheduler_init(NcBandwidthScheduler *scheduler) {
    if (scheduler) {
        scheduler->next_send_ns = 0;
    }
}

bool nc_bandwidth_scheduler_plan(const NcBandwidthScheduler *scheduler, uint64_t now_ns,
                                 uint64_t intended_delay_ns, size_t length,
                                 uint64_t bandwidth_kbps, uint64_t max_queue_ns,
                                 NcSendPlan *out) {
    uint64_t intended_due_ns;
    uint64_t packet_ns;
    uint64_t start_ns;
    uint64_t due_ns;
    uint64_t extra_ns;
    uint64_t expires_ns;
    if (!scheduler || !out) {
        return false;
    }
    intended_due_ns = sat_add(now_ns, intended_delay_ns);
    packet_ns = nc_packet_service_ns(length, bandwidth_kbps);
    start_ns = intended_due_ns > scheduler->next_send_ns ? intended_due_ns : scheduler->next_send_ns;
    due_ns = sat_add(start_ns, packet_ns);
    extra_ns = due_ns - intended_due_ns;
    if (max_queue_ns > 0 && extra_ns > max_queue_ns) {
        return false;
    }
    expires_ns = max_queue_ns > 0 ? sat_add(intended_due_ns, max_queue_ns) : 0;
    out->due_ns = due_ns;
    out->next_send_ns = due_ns;
    out->expires_ns = expires_ns;
    return true;
}

void nc_bandwidth_scheduler_commit(NcBandwidthScheduler *scheduler, const NcSendPlan *plan) {
    if (!scheduler || !plan) {
        return;
    }
    if (plan->next_send_ns > scheduler->next_send_ns) {
        scheduler->next_send_ns = plan->next_send_ns;
    }
}

uint64_t nc_paced_send_time(uint64_t due_ns, bool has_last, uint64_t last_send_end_ns,
                            uint64_t now_ns) {
    uint64_t send_at = due_ns;
    if (has_last && last_send_end_ns > send_at) {
        send_at = last_send_end_ns;
    }
    if (send_at < now_ns) {
        send_at = now_ns;
    }
    return send_at;
}

static bool stats_failed(const NcDirectionStats *stats) {
    return InterlockedCompareExchange((volatile LONG *)&stats->failed, 0, 0) != 0;
}

static void stats_add_sent(NcDirectionStats *stats) {
    InterlockedIncrement64(&stats->sent);
}

static void stats_add_dropped(NcDirectionStats *stats) {
    InterlockedIncrement64(&stats->dropped);
}

static bool stats_mark_failed(NcDirectionStats *stats) {
    return InterlockedExchange((volatile LONG *)&stats->failed, 1) == 0;
}

static void stats_set_queue(NcDirectionStats *stats, size_t count) {
    InterlockedExchange((volatile LONG *)&stats->queue, (LONG)count);
}

void nc_direction_stats(const NcDirectionEngine *engine, NcStatsSnapshot *out) {
    if (!engine || !out) {
        return;
    }
    out->sent = (uint64_t)InterlockedCompareExchange64(
        (volatile LONG64 *)&engine->stats.sent, 0, 0);
    out->dropped = (uint64_t)InterlockedCompareExchange64(
        (volatile LONG64 *)&engine->stats.dropped, 0, 0);
    out->queue = (uint32_t)InterlockedCompareExchange((volatile LONG *)&engine->stats.queue, 0, 0);
    out->failed = stats_failed(&engine->stats);
}

void nc_direction_reset_stats(NcDirectionEngine *engine) {
    if (!engine) {
        return;
    }
    InterlockedExchange64(&engine->stats.sent, 0);
    InterlockedExchange64(&engine->stats.dropped, 0);
    InterlockedExchange(&engine->stats.queue, 0);
}

static bool item_before(const QueuedItem *left, const QueuedItem *right) {
    if (left->due_ns != right->due_ns) {
        return left->due_ns < right->due_ns;
    }
    return left->sequence < right->sequence;
}

static bool queue_push(PacketQueue *queue, QueuedItem *item) {
    size_t index;
    if (queue->len == queue->cap) {
        size_t cap = queue->cap ? queue->cap * 2 : 64;
        QueuedItem **items = (QueuedItem **)realloc(queue->items, cap * sizeof(QueuedItem *));
        if (!items) {
            return false;
        }
        queue->items = items;
        queue->cap = cap;
    }
    index = queue->len++;
    queue->items[index] = item;
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!item_before(queue->items[index], queue->items[parent])) {
            break;
        }
        {
            QueuedItem *temp = queue->items[parent];
            queue->items[parent] = queue->items[index];
            queue->items[index] = temp;
        }
        index = parent;
    }
    return true;
}

static QueuedItem *queue_peek(const PacketQueue *queue) {
    return queue->len > 0 ? queue->items[0] : NULL;
}

static void queue_sift_down(PacketQueue *queue, size_t index) {
    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t smallest = index;
        QueuedItem *temp;
        if (left < queue->len && item_before(queue->items[left], queue->items[smallest])) {
            smallest = left;
        }
        if (right < queue->len && item_before(queue->items[right], queue->items[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        temp = queue->items[index];
        queue->items[index] = queue->items[smallest];
        queue->items[smallest] = temp;
        index = smallest;
    }
}

static QueuedItem *queue_pop(PacketQueue *queue) {
    QueuedItem *top;
    if (queue->len == 0) {
        return NULL;
    }
    top = queue->items[0];
    queue->len--;
    if (queue->len > 0) {
        queue->items[0] = queue->items[queue->len];
        queue_sift_down(queue, 0);
    }
    return top;
}

static INT_PTR current_handle(const NcDirectionEngine *engine) {
    return (INT_PTR)InterlockedCompareExchange64((volatile LONG64 *)&engine->windivert_handle, 0, 0);
}

static void report_fault(NcDirectionEngine *engine, const char *message) {
    NcEngineEvent event;
    if (InterlockedExchange(&engine->fault_reported, 1) != 0) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.kind = NC_ENGINE_EVENT_FAULT;
    snprintf(event.message, sizeof(event.message), "%s：%s", engine->display, message);
    nc_engine_events_send(engine->events, &event);
}

static void reporter_fail(NcDirectionEngine *engine, const char *message) {
    if (stats_mark_failed(&engine->stats)) {
        report_fault(engine, message);
    }
    nc_windivert_shutdown(current_handle(engine), NC_WINDIVERT_SHUTDOWN_RECV);
}

static bool reporter_send(NcDirectionEngine *engine, const void *packet, UINT length,
                          const void *address) {
    int error = 0;
    if (nc_windivert_send(current_handle(engine), packet, length, address, &error)) {
        stats_add_sent(&engine->stats);
        return true;
    }
    stats_add_dropped(&engine->stats);
    {
        char message[512];
        snprintf(message, sizeof(message), "发送数据包失败（Win32 错误 %d）。弱网模拟已停止。", error);
        reporter_fail(engine, message);
    }
    return false;
}

static bool protocol_matches(NcConditionProtocol protocol, uint8_t value) {
    switch (protocol) {
    case NC_PROTOCOL_TCP:
        return value == 6;
    case NC_PROTOCOL_UDP:
        return value == 17;
    case NC_PROTOCOL_ALL:
    default:
        return value == 6 || value == 17;
    }
}

static QueuedItem *rent_item(NcDirectionEngine *engine) {
    QueuedItem *item = NULL;
    EnterCriticalSection(&engine->pool_lock);
    if (engine->pool.len > 0) {
        item = engine->pool.items[--engine->pool.len];
    }
    LeaveCriticalSection(&engine->pool_lock);
    if (!item) {
        item = (QueuedItem *)calloc(1, sizeof(QueuedItem));
    }
    if (item) {
        item->length = 0;
        item->due_ns = 0;
        item->expires_ns = 0;
        item->sequence = 0;
    }
    return item;
}

static bool item_reserve(QueuedItem *item, size_t length) {
    if (item->cap >= length) {
        return true;
    }
    {
        unsigned char *data = (unsigned char *)realloc(item->data, length);
        if (!data) {
            return false;
        }
        item->data = data;
        item->cap = length;
    }
    return true;
}

static void return_item(NcDirectionEngine *engine, QueuedItem *item) {
    if (!item) {
        return;
    }
    item->length = 0;
    item->due_ns = 0;
    item->expires_ns = 0;
    item->sequence = 0;
    EnterCriticalSection(&engine->pool_lock);
    if (engine->pool.len < NC_POOL_LIMIT) {
        if (engine->pool.len == engine->pool.cap) {
            size_t cap = engine->pool.cap ? engine->pool.cap * 2 : 64;
            QueuedItem **items = (QueuedItem **)realloc(engine->pool.items, cap * sizeof(QueuedItem *));
            if (items) {
                engine->pool.items = items;
                engine->pool.cap = cap;
            }
        }
        if (engine->pool.len < engine->pool.cap) {
            engine->pool.items[engine->pool.len++] = item;
            item = NULL;
        }
    }
    LeaveCriticalSection(&engine->pool_lock);
    if (item) {
        free(item->data);
        free(item);
    }
}

static bool enqueue_packet(NcDirectionEngine *engine, const void *packet, size_t length,
                           const void *address, uint64_t due_ns, uint64_t expires_ns,
                           uint64_t sequence) {
    QueuedItem *item = rent_item(engine);
    uint64_t old_head_due = 0;
    bool had_head = false;
    bool inserted = false;
    if (!item) {
        return false;
    }
    if (!item_reserve(item, length)) {
        return_item(engine, item);
        return false;
    }
    memcpy(item->data, packet, length);
    memcpy(item->address, address, NC_WINDIVERT_ADDRESS_SIZE);
    item->length = length;
    item->due_ns = due_ns;
    item->expires_ns = expires_ns;
    item->sequence = sequence;
    EnterCriticalSection(&engine->queue_lock);
    if (engine->queue.len < NC_MAX_QUEUE_PACKETS &&
        engine->queue.bytes + length <= NC_MAX_QUEUE_BYTES) {
        QueuedItem *head = queue_peek(&engine->queue);
        if (head) {
            had_head = true;
            old_head_due = head->due_ns;
        }
        if (queue_push(&engine->queue, item)) {
            engine->queue.bytes += length;
            stats_set_queue(&engine->stats, engine->queue.len);
            inserted = true;
            if (!had_head || due_ns < old_head_due) {
                WakeConditionVariable(&engine->queue_cv);
            }
        }
    }
    LeaveCriticalSection(&engine->queue_lock);
    if (!inserted) {
        return_item(engine, item);
    }
    return inserted;
}

static void wait_until(NcDirectionEngine *engine, uint64_t target_ns) {
    for (;;) {
        uint64_t now;
        uint64_t wait_ns;
        DWORD wait_ms;
        if (InterlockedCompareExchange(&engine->stop, 0, 0) != 0 || stats_failed(&engine->stats)) {
            return;
        }
        now = nc_monotonic_ns();
        if (now >= target_ns) {
            return;
        }
        wait_ns = target_ns - now;
        if (wait_ns > (uint64_t)NC_WAIT_SLICE_MS * 1000000ull) {
            wait_ns = (uint64_t)NC_WAIT_SLICE_MS * 1000000ull;
        }
        wait_ms = (DWORD)((wait_ns + 999999ull) / 1000000ull);
        if (wait_ms == 0) {
            wait_ms = 1;
        }
        EnterCriticalSection(&engine->queue_lock);
        if (InterlockedCompareExchange(&engine->stop, 0, 0) == 0 && !stats_failed(&engine->stats)) {
            SleepConditionVariableCS(&engine->queue_cv, &engine->queue_lock, wait_ms);
        }
        LeaveCriticalSection(&engine->queue_lock);
    }
}

static void discard_all(NcDirectionEngine *engine) {
    QueuedItem *item;
    EnterCriticalSection(&engine->queue_lock);
    while ((item = queue_pop(&engine->queue)) != NULL) {
        engine->queue.bytes = engine->queue.bytes >= item->length ? engine->queue.bytes - item->length : 0;
        stats_add_dropped(&engine->stats);
        stats_set_queue(&engine->stats, engine->queue.len);
        LeaveCriticalSection(&engine->queue_lock);
        return_item(engine, item);
        EnterCriticalSection(&engine->queue_lock);
    }
    stats_set_queue(&engine->stats, 0);
    LeaveCriticalSection(&engine->queue_lock);
}

static unsigned __stdcall receiver_main(void *parameter) {
    NcDirectionEngine *engine = (NcDirectionEngine *)parameter;
    unsigned char *packet = (unsigned char *)malloc(NC_MAX_PACKET_SIZE);
    union {
        unsigned char bytes[NC_WINDIVERT_ADDRESS_BUFFER];
        uint64_t align;
    } address;
    NcBurstLossState burst;
    NcBandwidthScheduler scheduler;
    uint64_t sequence = 0;
    uint64_t random_state = nc_random_seed();
    uint64_t delay_ms;
    uint64_t jitter_ms;
    uint64_t bandwidth;
    int32_t loss_percent;
    INT_PTR handle = current_handle(engine);
    if (!packet) {
        InterlockedExchange(&engine->receiver_done, 1);
        WakeAllConditionVariable(&engine->queue_cv);
        return 1;
    }
    memset(address.bytes, 0, sizeof(address.bytes));
    nc_burst_loss_init(&burst, engine->profile.burst_allow_ms, engine->profile.burst_drop_ms,
                       nc_monotonic_ns() - engine->base_ns);
    nc_bandwidth_scheduler_init(&scheduler);
    delay_ms = engine->profile.delay_ms > 0 ? (uint64_t)engine->profile.delay_ms : 0;
    jitter_ms = engine->profile.jitter_ms > 0 ? (uint64_t)engine->profile.jitter_ms : 0;
    bandwidth = engine->profile.bandwidth_kbps > 0 ? (uint64_t)engine->profile.bandwidth_kbps : 0;
    loss_percent = engine->profile.random_loss_percent;
    if (loss_percent < 0) {
        loss_percent = 0;
    }
    if (loss_percent > 100) {
        loss_percent = 100;
    }
    for (;;) {
        UINT length = 0;
        int error = 0;
        uint8_t protocol_value = 0;
        if (stats_failed(&engine->stats)) {
            break;
        }
        if (!nc_windivert_recv(handle, packet, &length, address.bytes, &error)) {
            if (error == NC_ERROR_NO_DATA && InterlockedCompareExchange(&engine->stop, 0, 0) != 0) {
                break;
            }
            if (stats_failed(&engine->stats) || InterlockedCompareExchange(&engine->stop, 0, 0) != 0) {
                break;
            }
            {
                char message[512];
                snprintf(message, sizeof(message), "接收数据包失败（Win32 错误 %d）。弱网模拟已停止。", error);
                reporter_fail(engine, message);
            }
            break;
        }
        if (length == 0) {
            continue;
        }
        if (engine->protocol != NC_PROTOCOL_ALL || engine->process_filter) {
            NcFlowPacket flow;
            if (nc_flow_parse(packet, length, engine->outbound, &flow)) {
                protocol_value = flow.protocol;
                if (!protocol_matches(engine->protocol, protocol_value)) {
                    if (!reporter_send(engine, packet, length, address.bytes)) {
                        break;
                    }
                    continue;
                }
                if (engine->process_filter &&
                    !nc_process_filter_match(engine->process_filter, protocol_value, &flow.local,
                                             &flow.remote)) {
                    if (!reporter_send(engine, packet, length, address.bytes)) {
                        break;
                    }
                    continue;
                }
            } else if (engine->process_filter) {
                if (!reporter_send(engine, packet, length, address.bytes)) {
                    break;
                }
                continue;
            } else if (!nc_windivert_parse_protocol(packet, length, &protocol_value)) {
                if (!reporter_send(engine, packet, length, address.bytes)) {
                    break;
                }
                continue;
            } else if (!protocol_matches(engine->protocol, protocol_value)) {
                if (!reporter_send(engine, packet, length, address.bytes)) {
                    break;
                }
                continue;
            }
        }
        {
            uint64_t now = nc_monotonic_ns() - engine->base_ns;
            if (nc_burst_loss_should_drop(&burst, now)) {
                stats_add_dropped(&engine->stats);
                continue;
            }
        }
        if (loss_percent > 0 && nc_rand_below(&random_state, 100) < (uint32_t)loss_percent) {
            stats_add_dropped(&engine->stats);
            continue;
        }
        if (!engine->needs_queue) {
            if (!reporter_send(engine, packet, length, address.bytes)) {
                break;
            }
            continue;
        }
        {
            uint64_t jitter = jitter_ms > 0 ? nc_rand_inclusive(&random_state, jitter_ms) : 0;
            uint64_t intended_delay_ns = sat_add(delay_ms, jitter) * 1000000ull;
            uint64_t now = nc_monotonic_ns() - engine->base_ns;
            NcSendPlan plan;
            if (!nc_bandwidth_scheduler_plan(&scheduler, now, intended_delay_ns, length, bandwidth,
                                             engine->max_queue_ns, &plan)) {
                stats_add_dropped(&engine->stats);
                continue;
            }
            sequence++;
            if (enqueue_packet(engine, packet, length, address.bytes, plan.due_ns,
                               plan.expires_ns, sequence)) {
                nc_bandwidth_scheduler_commit(&scheduler, &plan);
            } else {
                stats_add_dropped(&engine->stats);
            }
        }
    }
    free(packet);
    InterlockedExchange(&engine->receiver_done, 1);
    WakeAllConditionVariable(&engine->queue_cv);
    return 0;
}

static unsigned __stdcall sender_main(void *parameter) {
    NcDirectionEngine *engine = (NcDirectionEngine *)parameter;
    uint64_t last_send_end_ns = 0;
    bool has_last = false;
    uint64_t bandwidth = engine->profile.bandwidth_kbps > 0
                             ? (uint64_t)engine->profile.bandwidth_kbps
                             : 0;
    for (;;) {
        QueuedItem *head;
        int action;
        bool stopping;
        bool failed;
        EnterCriticalSection(&engine->queue_lock);
        stopping = InterlockedCompareExchange(&engine->stop, 0, 0) != 0;
        failed = stats_failed(&engine->stats);
        if (stopping || failed) {
            QueuedItem *item = queue_pop(&engine->queue);
            if (item) {
                engine->queue.bytes = engine->queue.bytes >= item->length ? engine->queue.bytes - item->length : 0;
                stats_set_queue(&engine->stats, engine->queue.len);
                LeaveCriticalSection(&engine->queue_lock);
                if (failed || stopping) {
                    if (failed) {
                        stats_add_dropped(&engine->stats);
                    } else if (!reporter_send(engine, item->data, (UINT)item->length, item->address)) {
                        /* failure already counted */
                    }
                }
                return_item(engine, item);
                continue;
            }
            if (InterlockedCompareExchange(&engine->receiver_done, 0, 0) != 0) {
                LeaveCriticalSection(&engine->queue_lock);
                break;
            }
            LeaveCriticalSection(&engine->queue_lock);
            EnterCriticalSection(&engine->queue_lock);
            if (InterlockedCompareExchange(&engine->stop, 0, 0) == 0 && !stats_failed(&engine->stats)) {
                SleepConditionVariableCS(&engine->queue_cv, &engine->queue_lock, NC_WAIT_SLICE_MS);
            }
            LeaveCriticalSection(&engine->queue_lock);
            continue;
        }
        {
            uint64_t now = nc_monotonic_ns() - engine->base_ns;
            head = queue_peek(&engine->queue);
            if (head && head->expires_ns > 0 && now > head->expires_ns) {
                action = 1;
            } else if (head && head->due_ns > now) {
                action = 2;
            } else if (head) {
                action = 3;
            } else if (InterlockedCompareExchange(&engine->receiver_done, 0, 0) != 0) {
                action = 0;
            } else {
                action = 2;
            }
        }
        if (action == 0) {
            LeaveCriticalSection(&engine->queue_lock);
            break;
        }
        if (action == 1) {
            QueuedItem *item = queue_pop(&engine->queue);
            engine->queue.bytes = engine->queue.bytes >= item->length ? engine->queue.bytes - item->length : 0;
            stats_set_queue(&engine->stats, engine->queue.len);
            LeaveCriticalSection(&engine->queue_lock);
            stats_add_dropped(&engine->stats);
            return_item(engine, item);
            continue;
        }
        if (action == 2) {
            uint64_t now = nc_monotonic_ns() - engine->base_ns;
            uint64_t wait_ns = head ? (head->due_ns > now ? head->due_ns - now : 0)
                                    : (uint64_t)NC_WAIT_SLICE_MS * 1000000ull;
            DWORD wait_ms;
            if (wait_ns > (uint64_t)NC_WAIT_SLICE_MS * 1000000ull) {
                wait_ns = (uint64_t)NC_WAIT_SLICE_MS * 1000000ull;
            }
            wait_ms = (DWORD)((wait_ns + 999999ull) / 1000000ull);
            if (wait_ms == 0) {
                wait_ms = 1;
            }
            SleepConditionVariableCS(&engine->queue_cv, &engine->queue_lock, wait_ms);
            LeaveCriticalSection(&engine->queue_lock);
            continue;
        }
        {
            QueuedItem *item = queue_pop(&engine->queue);
            uint64_t packet_ns;
            uint64_t send_at;
            bool expired;
            engine->queue.bytes = engine->queue.bytes >= item->length ? engine->queue.bytes - item->length : 0;
            stats_set_queue(&engine->stats, engine->queue.len);
            LeaveCriticalSection(&engine->queue_lock);
            packet_ns = nc_packet_service_ns(item->length, bandwidth);
            send_at = item->due_ns;
            if (bandwidth > 0) {
                send_at = nc_paced_send_time(send_at, has_last, last_send_end_ns,
                                             nc_monotonic_ns() - engine->base_ns);
            }
            wait_until(engine, send_at);
            stopping = InterlockedCompareExchange(&engine->stop, 0, 0) != 0;
            failed = stats_failed(&engine->stats);
            expired = !stopping && item->expires_ns > 0 &&
                      (nc_monotonic_ns() - engine->base_ns) > item->expires_ns;
            if (failed || expired) {
                stats_add_dropped(&engine->stats);
            } else if (reporter_send(engine, item->data, (UINT)item->length, item->address)) {
                if (bandwidth > 0) {
                    last_send_end_ns = sat_add(send_at, packet_ns);
                    has_last = true;
                }
            }
            return_item(engine, item);
            continue;
        }
    }
    return 0;
}

NcDirectionEngine *nc_direction_new(const char *display_name, const char *filter, bool outbound,
                                    const NcDirectionProfile *profile,
                                    NcConditionProtocol protocol,
                                    NcProcessFlowFilter *process_filter,
                                    int32_t max_queue_delay_ms, NcEngineEvents *events) {
    NcDirectionEngine *engine = (NcDirectionEngine *)calloc(1, sizeof(NcDirectionEngine));
    if (!engine) {
        return NULL;
    }
    snprintf(engine->display, sizeof(engine->display), "%s", display_name ? display_name : "方向");
    engine->filter = nc_strdup(filter ? filter : "false");
    engine->outbound = outbound;
    if (profile) {
        engine->profile = *profile;
    }
    engine->protocol = protocol;
    engine->process_filter = process_filter;
    engine->events = events;
    engine->max_queue_ns = max_queue_delay_ms > 0 ? (uint64_t)max_queue_delay_ms * 1000000ull : 0;
    engine->needs_queue = profile && (profile->delay_ms > 0 || profile->jitter_ms > 0 ||
                                      profile->bandwidth_kbps > 0);
    InitializeCriticalSection(&engine->queue_lock);
    InitializeConditionVariable(&engine->queue_cv);
    InitializeCriticalSection(&engine->pool_lock);
    return engine;
}

static void clear_thread(HANDLE *thread) {
    if (*thread) {
        CloseHandle(*thread);
        *thread = NULL;
    }
}

bool nc_direction_start(NcDirectionEngine *engine, NcErr *err) {
    INT_PTR handle = 0;
    if (!engine) {
        return nc_fail(err, "方向引擎无效。");
    }
    if (engine->start_attempted) {
        return nc_fail(err, "DirectionEngine 只能启动一次，停止后请创建新实例。");
    }
    engine->start_attempted = true;
    if (!nc_windivert_open(engine->filter, &handle, err)) {
        return false;
    }
    nc_windivert_set_params(handle);
    InterlockedExchange64(&engine->windivert_handle, (LONG64)handle);
    engine->base_ns = nc_monotonic_ns();
    engine->receiver_thread = (HANDLE)_beginthreadex(NULL, 0, receiver_main, engine, 0, NULL);
    if (!engine->receiver_thread) {
        DWORD code = GetLastError();
        nc_windivert_close(handle);
        InterlockedExchange64(&engine->windivert_handle, 0);
        return nc_fail(err, "启动%s接收线程失败（Win32 错误 %lu）。", engine->display,
                       (unsigned long)code);
    }
    engine->sender_thread = (HANDLE)_beginthreadex(NULL, 0, sender_main, engine, 0, NULL);
    if (!engine->sender_thread) {
        DWORD code = GetLastError();
        InterlockedExchange(&engine->stop, 1);
        nc_windivert_shutdown(handle, NC_WINDIVERT_SHUTDOWN_RECV);
        WakeAllConditionVariable(&engine->queue_cv);
        WaitForSingleObject(engine->receiver_thread, INFINITE);
        clear_thread(&engine->receiver_thread);
        nc_windivert_close(handle);
        InterlockedExchange64(&engine->windivert_handle, 0);
        discard_all(engine);
        return nc_fail(err, "启动%s发送线程失败（Win32 错误 %lu）。", engine->display,
                       (unsigned long)code);
    }
    engine->started = true;
    return true;
}

void nc_direction_stop(NcDirectionEngine *engine) {
    INT_PTR handle;
    if (!engine || !engine->started) {
        return;
    }
    InterlockedExchange(&engine->stop, 1);
    handle = current_handle(engine);
    if (handle) {
        if (!nc_windivert_shutdown(handle, NC_WINDIVERT_SHUTDOWN_RECV)) {
            nc_windivert_close(handle);
            InterlockedExchange64(&engine->windivert_handle, 0);
        }
    }
    WakeAllConditionVariable(&engine->queue_cv);
    if (engine->receiver_thread) {
        WaitForSingleObject(engine->receiver_thread, INFINITE);
        clear_thread(&engine->receiver_thread);
    }
    if (engine->sender_thread) {
        WaitForSingleObject(engine->sender_thread, INFINITE);
        clear_thread(&engine->sender_thread);
    }
    handle = (INT_PTR)InterlockedExchange64(&engine->windivert_handle, 0);
    if (handle) {
        nc_windivert_close(handle);
    }
    discard_all(engine);
    engine->started = false;
}

void nc_direction_free(NcDirectionEngine *engine) {
    size_t index;
    if (!engine) {
        return;
    }
    nc_direction_stop(engine);
    while (engine->queue.len > 0) {
        QueuedItem *item = queue_pop(&engine->queue);
        if (item) {
            free(item->data);
            free(item);
        }
    }
    free(engine->queue.items);
    for (index = 0; index < engine->pool.len; index++) {
        free(engine->pool.items[index]->data);
        free(engine->pool.items[index]);
    }
    free(engine->pool.items);
    DeleteCriticalSection(&engine->queue_lock);
    DeleteCriticalSection(&engine->pool_lock);
    free(engine->filter);
    free(engine);
}