#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    NC_ENGINE_EVENT_START_DONE = 0,
    NC_ENGINE_EVENT_STOP_DONE,
    NC_ENGINE_EVENT_FAULT
} NcEngineEventKind;

typedef struct {
    NcEngineEventKind kind;
    bool ok;
    char message[1024];
} NcEngineEvent;

typedef struct NcEngineEvents NcEngineEvents;

NcEngineEvents *nc_engine_events_new(HWND wake_window, UINT wake_message);
void nc_engine_events_free(NcEngineEvents *events);
void nc_engine_events_send(NcEngineEvents *events, const NcEngineEvent *event);
bool nc_engine_events_pop(NcEngineEvents *events, NcEngineEvent *out);

typedef struct {
    uint64_t sent;
    uint64_t dropped;
    uint32_t queue;
    bool failed;
} NcStatsSnapshot;

typedef struct NcNetworkConditioner NcNetworkConditioner;

extern const char *const NC_DEFAULT_OUTBOUND_FILTER;
extern const char *const NC_DEFAULT_INBOUND_FILTER;

NcNetworkConditioner *nc_conditioner_new(NcEngineEvents *events);
void nc_conditioner_free(NcNetworkConditioner *conditioner);
bool nc_conditioner_start(NcNetworkConditioner *conditioner, const NcProfile *profile,
                          const char *outbound_filter, const char *inbound_filter,
                          int32_t max_queue_delay_ms, const char *const *process_names,
                          size_t process_count, NcErr *err);
void nc_conditioner_stop(NcNetworkConditioner *conditioner);
bool nc_conditioner_uplink_stats(NcNetworkConditioner *conditioner, NcStatsSnapshot *out);
bool nc_conditioner_downlink_stats(NcNetworkConditioner *conditioner, NcStatsSnapshot *out);
void nc_conditioner_reset_stats(NcNetworkConditioner *conditioner);
