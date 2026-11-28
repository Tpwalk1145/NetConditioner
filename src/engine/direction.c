#include "engine/direction.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/str.h"
#include "core/time.h"
#include "engine/windivert.h"

#define NC_ERROR_NO_DATA 232
#define NC_WAIT_SLICE_MS 100u

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef struct {
    volatile LONG64 sent;
    volatile LONG64 dropped;
    volatile LONG queue;
    volatile LONG failed;
} DirectionStats;

struct NcDirectionEngine {
    char display[32];
    char *filter;
    bool outbound;
    NcDirectionProfile profile;
    NcConditionProtocol protocol;
    NcProcessFlowFilter *process_filter;
    NcEngineEvents *events;
    DirectionStats stats;
    uint64_t max_queue_ns;
    bool needs_queue;
    volatile LONG64 windivert_handle;
    CRITICAL_SECTION queue_lock;
    NcPacketQueue queue;
    CRITICAL_SECTION pool_lock;
    NcPacketPool pool;
    HANDLE stop_event;
    HANDLE wake_event;
    volatile LONG stop;
    volatile LONG receiver_done;
    volatile LONG fault_reported;
    HANDLE receiver_thread;
    HANDLE sender_thread;
    bool started;
    bool start_attempted;
    uint64_t base_ns;
};

static bool stats_failed(const DirectionStats *stats) {
    return InterlockedCompareExchange((volatile LONG *)&stats->failed, 0, 0) != 0;
}

static void stats_add_sent(DirectionStats *stats) {
    InterlockedIncrement64(&stats->sent);
}

static void stats_add_dropped(DirectionStats *stats) {
    InterlockedIncrement64(&stats->dropped);
}

static bool stats_mark_failed(DirectionStats *stats) {
    return InterlockedExchange((volatile LONG *)&stats->failed, 1) == 0;
}

static void stats_set_queue(DirectionStats *stats, size_t count) {
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

static uint64_t engine_now_ns(const NcDirectionEngine *engine) {
    uint64_t raw = nc_monotonic_ns();
    return raw >= engine->base_ns ? raw - engine->base_ns : 0;
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
    if (engine->stop_event) {
        SetEvent(engine->stop_event);
    }
    if (engine->wake_event) {
        SetEvent(engine->wake_event);
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

static NcQueuedPacket *rent_packet(NcDirectionEngine *engine) {
    NcQueuedPacket *item;
    EnterCriticalSection(&engine->pool_lock);
    item = nc_packet_pool_rent(&engine->pool);
    LeaveCriticalSection(&engine->pool_lock);
    return item;
}

static void return_packet(NcDirectionEngine *engine, NcQueuedPacket *item) {
    if (!item) {
        return;
    }
    EnterCriticalSection(&engine->pool_lock);
    nc_packet_pool_return(&engine->pool, item);
    LeaveCriticalSection(&engine->pool_lock);
}

static bool enqueue_packet(NcDirectionEngine *engine, const void *packet, size_t length,
                           const void *address, uint64_t due_ns, uint64_t expires_ns,
                           uint64_t sequence) {
    NcQueuedPacket *item = rent_packet(engine);
    uint64_t old_head_due = 0;
    bool had_head = false;
    bool inserted = false;
    bool should_wake = false;
    if (!item) {
        return false;
    }
    if (!nc_packet_reserve(item, length)) {
        return_packet(engine, item);
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
        NcQueuedPacket *head = nc_packet_queue_peek(&engine->queue);
        if (head) {
            had_head = true;
            old_head_due = head->due_ns;
        }
        if (nc_packet_queue_push(&engine->queue, item)) {
            stats_set_queue(&engine->stats, engine->queue.len);
            inserted = true;
            should_wake = !had_head || due_ns < old_head_due;
        }
    }
    LeaveCriticalSection(&engine->queue_lock);
    if (should_wake && engine->wake_event) {
        SetEvent(engine->wake_event);
    }
    if (!inserted) {
        return_packet(engine, item);
    }
    return inserted;
}

static HANDLE create_pacing_timer(void) {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    return timer;
}

static void wait_slice_ns(HANDLE timer, HANDLE stop_event, HANDLE wake_event, uint64_t wait_ns) {
    uint64_t clamped = wait_ns > (uint64_t)NC_WAIT_SLICE_MS * 1000000ull
                           ? (uint64_t)NC_WAIT_SLICE_MS * 1000000ull
                           : wait_ns;
    if (clamped == 0) {
        return;
    }
    if (timer) {
        LARGE_INTEGER due;
        LONGLONG ticks = (LONGLONG)((clamped + 99ull) / 100ull);
        HANDLE handles[3];
        DWORD count = 0;
        if (ticks < 1) {
            ticks = 1;
        }
        due.QuadPart = -ticks;
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            if (stop_event) {
                handles[count++] = stop_event;
            }
            if (wake_event) {
                handles[count++] = wake_event;
            }
            handles[count++] = timer;
            WaitForMultipleObjects(count, handles, FALSE, INFINITE);
            return;
        }
    }
    {
        DWORD wait_ms = (DWORD)((clamped + 999999ull) / 1000000ull);
        HANDLE handles[2];
        DWORD count = 0;
        if (wait_ms == 0) {
            wait_ms = 1;
        }
        if (stop_event) {
            handles[count++] = stop_event;
        }
        if (wake_event) {
            handles[count++] = wake_event;
        }
        if (count > 0) {
            WaitForMultipleObjects(count, handles, FALSE, wait_ms);
        } else {
            Sleep(wait_ms);
        }
    }
}

static void wait_until(NcDirectionEngine *engine, HANDLE timer, uint64_t target_ns) {
    for (;;) {
        uint64_t now;
        if (InterlockedCompareExchange(&engine->stop, 0, 0) != 0 || stats_failed(&engine->stats)) {
            return;
        }
        now = engine_now_ns(engine);
        if (now >= target_ns) {
            return;
        }
        wait_slice_ns(timer, engine->stop_event, NULL, target_ns - now);
    }
}

static void discard_all(NcDirectionEngine *engine) {
    NcQueuedPacket *item;
    EnterCriticalSection(&engine->queue_lock);
    while ((item = nc_packet_queue_pop(&engine->queue)) != NULL) {
        stats_add_dropped(&engine->stats);
        stats_set_queue(&engine->stats, engine->queue.len);
        LeaveCriticalSection(&engine->queue_lock);
        return_packet(engine, item);
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
        if (engine->wake_event) {
            SetEvent(engine->wake_event);
        }
        return 1;
    }
    memset(address.bytes, 0, sizeof(address.bytes));
    nc_burst_loss_init(&burst, engine->profile.burst_allow_ms, engine->profile.burst_drop_ms,
                       engine_now_ns(engine));
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
            uint64_t now = engine_now_ns(engine);
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
            uint64_t intended_delay_ns = nc_sat_add_u64(delay_ms, jitter) * 1000000ull;
            uint64_t now = engine_now_ns(engine);
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
    if (engine->wake_event) {
        SetEvent(engine->wake_event);
    }
    return 0;
}

static unsigned __stdcall sender_main(void *parameter) {
    NcDirectionEngine *engine = (NcDirectionEngine *)parameter;
    HANDLE timer = create_pacing_timer();
    uint64_t last_send_end_ns = 0;
    bool has_last = false;
    uint64_t bandwidth = engine->profile.bandwidth_kbps > 0
                             ? (uint64_t)engine->profile.bandwidth_kbps
                             : 0;
    for (;;) {
        NcQueuedPacket *head;
        int action;
        bool stopping;
        bool failed;
        uint64_t head_due = 0;
        EnterCriticalSection(&engine->queue_lock);
        stopping = InterlockedCompareExchange(&engine->stop, 0, 0) != 0;
        failed = stats_failed(&engine->stats);
        if (stopping || failed) {
            NcQueuedPacket *item = nc_packet_queue_pop(&engine->queue);
            if (item) {
                stats_set_queue(&engine->stats, engine->queue.len);
                LeaveCriticalSection(&engine->queue_lock);
                if (failed) {
                    stats_add_dropped(&engine->stats);
                } else if (!reporter_send(engine, item->data, (UINT)item->length, item->address)) {
                    /* failure already counted */
                }
                return_packet(engine, item);
                continue;
            }
            if (InterlockedCompareExchange(&engine->receiver_done, 0, 0) != 0) {
                LeaveCriticalSection(&engine->queue_lock);
                break;
            }
            LeaveCriticalSection(&engine->queue_lock);
            if (engine->wake_event) {
                WaitForSingleObject(engine->wake_event, NC_WAIT_SLICE_MS);
            } else {
                Sleep(1);
            }
            continue;
        }
        {
            uint64_t now = engine_now_ns(engine);
            head = nc_packet_queue_peek(&engine->queue);
            if (head && head->expires_ns > 0 && now > head->expires_ns) {
                action = 1;
            } else if (head && head->due_ns > now) {
                action = 2;
                head_due = head->due_ns;
            } else if (head) {
                action = 3;
            } else if (InterlockedCompareExchange(&engine->receiver_done, 0, 0) != 0) {
                action = 0;
            } else {
                action = 2;
                head_due = 0;
            }
        }
        if (action == 0) {
            LeaveCriticalSection(&engine->queue_lock);
            break;
        }
        if (action == 1) {
            NcQueuedPacket *item = nc_packet_queue_pop(&engine->queue);
            stats_set_queue(&engine->stats, engine->queue.len);
            LeaveCriticalSection(&engine->queue_lock);
            stats_add_dropped(&engine->stats);
            return_packet(engine, item);
            continue;
        }
        if (action == 2) {
            uint64_t now = engine_now_ns(engine);
            uint64_t wait_ns = head_due > 0
                                   ? (head_due > now ? head_due - now : 0)
                                   : (uint64_t)NC_WAIT_SLICE_MS * 1000000ull;
            LeaveCriticalSection(&engine->queue_lock);
            wait_slice_ns(timer, engine->stop_event, engine->wake_event, wait_ns);
            continue;
        }
        {
            NcQueuedPacket *item = nc_packet_queue_pop(&engine->queue);
            uint64_t packet_ns;
            uint64_t send_at;
            bool expired;
            stats_set_queue(&engine->stats, engine->queue.len);
            LeaveCriticalSection(&engine->queue_lock);
            packet_ns = nc_packet_service_ns(item->length, bandwidth);
            send_at = item->due_ns;
            if (bandwidth > 0) {
                send_at = nc_paced_send_time(send_at, has_last, last_send_end_ns,
                                             engine_now_ns(engine));
            }
            wait_until(engine, timer, send_at);
            stopping = InterlockedCompareExchange(&engine->stop, 0, 0) != 0;
            failed = stats_failed(&engine->stats);
            expired = !stopping && item->expires_ns > 0 && engine_now_ns(engine) > item->expires_ns;
            if (failed || expired) {
                stats_add_dropped(&engine->stats);
            } else if (reporter_send(engine, item->data, (UINT)item->length, item->address)) {
                if (bandwidth > 0) {
                    last_send_end_ns = nc_sat_add_u64(send_at, packet_ns);
                    has_last = true;
                }
            }
            return_packet(engine, item);
        }
    }
    if (timer) {
        CloseHandle(timer);
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
    InitializeCriticalSection(&engine->pool_lock);
    nc_packet_queue_init(&engine->queue);
    nc_packet_pool_init(&engine->pool);
    engine->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    engine->wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!engine->stop_event || !engine->wake_event) {
        nc_direction_free(engine);
        return NULL;
    }
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
    if (engine->needs_queue) {
        engine->sender_thread = (HANDLE)_beginthreadex(NULL, 0, sender_main, engine, 0, NULL);
        if (!engine->sender_thread) {
            DWORD code = GetLastError();
            InterlockedExchange(&engine->stop, 1);
            SetEvent(engine->stop_event);
            SetEvent(engine->wake_event);
            nc_windivert_shutdown(handle, NC_WINDIVERT_SHUTDOWN_RECV);
            WaitForSingleObject(engine->receiver_thread, INFINITE);
            clear_thread(&engine->receiver_thread);
            nc_windivert_close(handle);
            InterlockedExchange64(&engine->windivert_handle, 0);
            discard_all(engine);
            return nc_fail(err, "启动%s发送线程失败（Win32 错误 %lu）。", engine->display,
                           (unsigned long)code);
        }
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
    if (engine->stop_event) {
        SetEvent(engine->stop_event);
    }
    if (engine->wake_event) {
        SetEvent(engine->wake_event);
    }
    handle = current_handle(engine);
    if (handle) {
        if (!nc_windivert_shutdown(handle, NC_WINDIVERT_SHUTDOWN_RECV)) {
            nc_windivert_close(handle);
            InterlockedExchange64(&engine->windivert_handle, 0);
        }
    }
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
    if (!engine) {
        return;
    }
    nc_direction_stop(engine);
    nc_packet_queue_free(&engine->queue);
    nc_packet_pool_free(&engine->pool);
    if (engine->stop_event) {
        CloseHandle(engine->stop_event);
    }
    if (engine->wake_event) {
        CloseHandle(engine->wake_event);
    }
    DeleteCriticalSection(&engine->queue_lock);
    DeleteCriticalSection(&engine->pool_lock);
    free(engine->filter);
    free(engine);
}
