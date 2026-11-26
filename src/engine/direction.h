#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "config.h"
#include "engine.h"
#include "flow.h"
#include "process_filter.h"

typedef struct {
    uint64_t allow_ns;
    uint64_t drop_ns;
    uint64_t phase_start_ns;
} NcBurstLossState;

typedef struct {
    uint64_t next_send_ns;
} NcBandwidthScheduler;

typedef struct {
    uint64_t due_ns;
    uint64_t next_send_ns;
    uint64_t expires_ns;
} NcSendPlan;

void nc_burst_loss_init(NcBurstLossState *state, int32_t allow_ms, int32_t drop_ms, uint64_t now_ns);
bool nc_burst_loss_should_drop(const NcBurstLossState *state, uint64_t now_ns);
uint64_t nc_packet_service_ns(size_t length, uint64_t bandwidth_kbps);
void nc_bandwidth_scheduler_init(NcBandwidthScheduler *scheduler);
bool nc_bandwidth_scheduler_plan(const NcBandwidthScheduler *scheduler, uint64_t now_ns,
                                 uint64_t intended_delay_ns, size_t length,
                                 uint64_t bandwidth_kbps, uint64_t max_queue_ns,
                                 NcSendPlan *out);
void nc_bandwidth_scheduler_commit(NcBandwidthScheduler *scheduler, const NcSendPlan *plan);
uint64_t nc_paced_send_time(uint64_t due_ns, bool has_last, uint64_t last_send_end_ns,
                            uint64_t now_ns);

typedef struct {
    volatile LONG64 sent;
    volatile LONG64 dropped;
    volatile LONG queue;
    volatile LONG failed;
} NcDirectionStats;

typedef struct NcDirectionEngine NcDirectionEngine;

NcDirectionEngine *nc_direction_new(const char *display_name, const char *filter, bool outbound,
                                    const NcDirectionProfile *profile,
                                    NcConditionProtocol protocol,
                                    NcProcessFlowFilter *process_filter,
                                    int32_t max_queue_delay_ms, NcEngineEvents *events);
bool nc_direction_start(NcDirectionEngine *engine, NcErr *err);
void nc_direction_stop(NcDirectionEngine *engine);
void nc_direction_free(NcDirectionEngine *engine);
void nc_direction_stats(const NcDirectionEngine *engine, NcStatsSnapshot *out);
void nc_direction_reset_stats(NcDirectionEngine *engine);
