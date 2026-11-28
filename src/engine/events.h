#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

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
