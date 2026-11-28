#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "config/config.h"
#include "engine/engine.h"

typedef struct {
    void *userdata;
    void (*on_state_changed)(void *userdata);
    void (*on_start_succeeded)(void *userdata);
    void (*on_start_failed)(void *userdata, const char *message);
    void (*on_stop_finished)(void *userdata, bool silent);
    void (*on_fault)(void *userdata, const char *message);
} NcEngineRunnerCallbacks;

typedef struct NcEngineRunner NcEngineRunner;

NcEngineRunner *nc_engine_runner_new(HWND wake_hwnd, UINT wake_message,
                                     const NcEngineRunnerCallbacks *callbacks);
void nc_engine_runner_free(NcEngineRunner *runner);

bool nc_engine_runner_is_running(const NcEngineRunner *runner);
bool nc_engine_runner_is_busy(const NcEngineRunner *runner);
bool nc_engine_runner_start_async(NcEngineRunner *runner, const NcProfile *profile,
                                  const NcSettings *settings);
bool nc_engine_runner_stop_async(NcEngineRunner *runner, bool silent);
void nc_engine_runner_stop_sync(NcEngineRunner *runner);
void nc_engine_runner_reschedule_auto_stop(NcEngineRunner *runner, int32_t max_run_minutes);
bool nc_engine_runner_auto_stop_due(NcEngineRunner *runner);
void nc_engine_runner_stats(NcEngineRunner *runner, NcStatsSnapshot *uplink,
                            NcStatsSnapshot *downlink);
void nc_engine_runner_drain(NcEngineRunner *runner, int32_t max_run_minutes);
