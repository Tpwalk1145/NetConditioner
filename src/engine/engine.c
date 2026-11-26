#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "direction.h"
#include "process_filter.h"

const char *const NC_DEFAULT_OUTBOUND_FILTER = "outbound and !loopback and (tcp or udp)";
const char *const NC_DEFAULT_INBOUND_FILTER = "inbound and !loopback and (tcp or udp)";

typedef struct NcEventNode {
    NcEngineEvent event;
    struct NcEventNode *next;
} NcEventNode;

struct NcEngineEvents {
    SRWLOCK lock;
    NcEventNode *head;
    NcEventNode *tail;
    HWND wake;
    UINT message;
};

NcEngineEvents *nc_engine_events_new(HWND wake_window, UINT wake_message) {
    NcEngineEvents *events = (NcEngineEvents *)calloc(1, sizeof(NcEngineEvents));
    if (!events) {
        return NULL;
    }
    InitializeSRWLock(&events->lock);
    events->wake = wake_window;
    events->message = wake_message;
    return events;
}

void nc_engine_events_free(NcEngineEvents *events) {
    NcEventNode *node;
    if (!events) {
        return;
    }
    AcquireSRWLockExclusive(&events->lock);
    node = events->head;
    events->head = NULL;
    events->tail = NULL;
    ReleaseSRWLockExclusive(&events->lock);
    while (node) {
        NcEventNode *next = node->next;
        free(node);
        node = next;
    }
    free(events);
}

void nc_engine_events_send(NcEngineEvents *events, const NcEngineEvent *event) {
    NcEventNode *node;
    if (!events || !event) {
        return;
    }
    node = (NcEventNode *)malloc(sizeof(NcEventNode));
    if (!node) {
        return;
    }
    node->event = *event;
    node->next = NULL;
    AcquireSRWLockExclusive(&events->lock);
    if (events->tail) {
        events->tail->next = node;
    } else {
        events->head = node;
    }
    events->tail = node;
    ReleaseSRWLockExclusive(&events->lock);
    if (events->wake) {
        PostMessageW(events->wake, events->message, 0, 0);
    }
}

bool nc_engine_events_pop(NcEngineEvents *events, NcEngineEvent *out) {
    NcEventNode *node;
    if (!events || !out) {
        return false;
    }
    AcquireSRWLockExclusive(&events->lock);
    node = events->head;
    if (node) {
        events->head = node->next;
        if (!events->head) {
            events->tail = NULL;
        }
    }
    ReleaseSRWLockExclusive(&events->lock);
    if (!node) {
        return false;
    }
    *out = node->event;
    free(node);
    return true;
}

struct NcNetworkConditioner {
    CRITICAL_SECTION lifecycle_lock;
    CRITICAL_SECTION state_lock;
    NcDirectionEngine *uplink;
    NcDirectionEngine *downlink;
    NcProcessFlowFilter *process_filter;
    bool running;
    NcEngineEvents *events;
};

NcNetworkConditioner *nc_conditioner_new(NcEngineEvents *events) {
    NcNetworkConditioner *conditioner =
        (NcNetworkConditioner *)calloc(1, sizeof(NcNetworkConditioner));
    if (!conditioner) {
        return NULL;
    }
    InitializeCriticalSection(&conditioner->lifecycle_lock);
    InitializeCriticalSection(&conditioner->state_lock);
    conditioner->events = events;
    return conditioner;
}

void nc_conditioner_free(NcNetworkConditioner *conditioner) {
    if (!conditioner) {
        return;
    }
    nc_conditioner_stop(conditioner);
    DeleteCriticalSection(&conditioner->lifecycle_lock);
    DeleteCriticalSection(&conditioner->state_lock);
    free(conditioner);
}

static void abandon_partial([[maybe_unused]] NcNetworkConditioner *conditioner,
                            NcDirectionEngine *uplink, NcDirectionEngine *downlink,
                            NcProcessFlowFilter *filter) {
    if (uplink) {
        nc_direction_stop(uplink);
        nc_direction_free(uplink);
    }
    if (downlink) {
        nc_direction_stop(downlink);
        nc_direction_free(downlink);
    }
    if (filter) {
        nc_process_filter_shutdown(filter);
        nc_process_filter_free(filter);
    }
}

bool nc_conditioner_start(NcNetworkConditioner *conditioner, const NcProfile *profile,
                          const char *outbound_filter, const char *inbound_filter,
                          int32_t max_queue_delay_ms, const char *const *process_names,
                          size_t process_count, NcErr *err) {
    NcDirectionEngine *uplink = NULL;
    NcDirectionEngine *downlink = NULL;
    NcProcessFlowFilter *filter = NULL;
    if (!conditioner || !profile) {
        return nc_fail(err, "弱网配置无效。");
    }
    EnterCriticalSection(&conditioner->lifecycle_lock);
    EnterCriticalSection(&conditioner->state_lock);
    if (conditioner->running) {
        LeaveCriticalSection(&conditioner->state_lock);
        LeaveCriticalSection(&conditioner->lifecycle_lock);
        return nc_fail(err, "弱网模拟已经在运行。");
    }
    if (process_names && process_count > 0) {
        filter = nc_process_filter_new(process_names, process_count);
        if (!filter) {
            LeaveCriticalSection(&conditioner->state_lock);
            LeaveCriticalSection(&conditioner->lifecycle_lock);
            return nc_fail(err, "初始化目标进程过滤失败。");
        }
    }
    uplink = nc_direction_new("上行",
                              outbound_filter ? outbound_filter : NC_DEFAULT_OUTBOUND_FILTER, true,
                              &profile->uplink, profile->protocol, filter, max_queue_delay_ms,
                              conditioner->events);
    downlink = nc_direction_new("下行",
                                inbound_filter ? inbound_filter : NC_DEFAULT_INBOUND_FILTER, false,
                                &profile->downlink, profile->protocol, filter, max_queue_delay_ms,
                                conditioner->events);
    if (!uplink || !downlink) {
        abandon_partial(conditioner, uplink, downlink, filter);
        LeaveCriticalSection(&conditioner->state_lock);
        LeaveCriticalSection(&conditioner->lifecycle_lock);
        return nc_fail(err, "创建弱网引擎失败。");
    }
    if (!nc_direction_start(uplink, err)) {
        abandon_partial(conditioner, uplink, downlink, filter);
        LeaveCriticalSection(&conditioner->state_lock);
        LeaveCriticalSection(&conditioner->lifecycle_lock);
        return false;
    }
    if (!nc_direction_start(downlink, err)) {
        abandon_partial(conditioner, uplink, downlink, filter);
        LeaveCriticalSection(&conditioner->state_lock);
        LeaveCriticalSection(&conditioner->lifecycle_lock);
        return false;
    }
    conditioner->uplink = uplink;
    conditioner->downlink = downlink;
    conditioner->process_filter = filter;
    conditioner->running = true;
    LeaveCriticalSection(&conditioner->state_lock);
    LeaveCriticalSection(&conditioner->lifecycle_lock);
    return true;
}

void nc_conditioner_stop(NcNetworkConditioner *conditioner) {
    NcDirectionEngine *uplink;
    NcDirectionEngine *downlink;
    NcProcessFlowFilter *filter;
    if (!conditioner) {
        return;
    }
    EnterCriticalSection(&conditioner->lifecycle_lock);
    EnterCriticalSection(&conditioner->state_lock);
    if (!conditioner->running) {
        LeaveCriticalSection(&conditioner->state_lock);
        LeaveCriticalSection(&conditioner->lifecycle_lock);
        return;
    }
    uplink = conditioner->uplink;
    downlink = conditioner->downlink;
    filter = conditioner->process_filter;
    conditioner->uplink = NULL;
    conditioner->downlink = NULL;
    conditioner->process_filter = NULL;
    conditioner->running = false;
    LeaveCriticalSection(&conditioner->state_lock);
    if (uplink) {
        nc_direction_stop(uplink);
        nc_direction_free(uplink);
    }
    if (downlink) {
        nc_direction_stop(downlink);
        nc_direction_free(downlink);
    }
    if (filter) {
        nc_process_filter_shutdown(filter);
        nc_process_filter_free(filter);
    }
    LeaveCriticalSection(&conditioner->lifecycle_lock);
}

bool nc_conditioner_uplink_stats(NcNetworkConditioner *conditioner, NcStatsSnapshot *out) {
    bool ok = false;
    if (!conditioner || !out) {
        return false;
    }
    EnterCriticalSection(&conditioner->state_lock);
    if (conditioner->running && conditioner->uplink) {
        nc_direction_stats(conditioner->uplink, out);
        ok = true;
    }
    LeaveCriticalSection(&conditioner->state_lock);
    return ok;
}

bool nc_conditioner_downlink_stats(NcNetworkConditioner *conditioner, NcStatsSnapshot *out) {
    bool ok = false;
    if (!conditioner || !out) {
        return false;
    }
    EnterCriticalSection(&conditioner->state_lock);
    if (conditioner->running && conditioner->downlink) {
        nc_direction_stats(conditioner->downlink, out);
        ok = true;
    }
    LeaveCriticalSection(&conditioner->state_lock);
    return ok;
}

void nc_conditioner_reset_stats(NcNetworkConditioner *conditioner) {
    if (!conditioner) {
        return;
    }
    EnterCriticalSection(&conditioner->state_lock);
    if (conditioner->running) {
        if (conditioner->uplink) {
            nc_direction_reset_stats(conditioner->uplink);
        }
        if (conditioner->downlink) {
            nc_direction_reset_stats(conditioner->downlink);
        }
    }
    LeaveCriticalSection(&conditioner->state_lock);
}
