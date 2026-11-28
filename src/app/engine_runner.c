#include "app/engine_runner.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/str.h"
#include "core/time.h"

struct NcEngineRunner {
    NcNetworkConditioner *engine;
    NcEngineEvents *events;
    NcEngineRunnerCallbacks callbacks;
    CRITICAL_SECTION worker_lock;
    CONDITION_VARIABLE worker_cv;
    int worker_count;
    bool running;
    bool busy;
    bool stop_silent;
    bool has_run_started;
    uint64_t run_started_ns;
    bool has_auto_stop;
    uint64_t auto_stop_ns;
};

typedef struct {
    NcEngineRunner *runner;
    NcProfile profile;
    char **names;
    size_t name_count;
    int32_t max_queue_delay_ms;
} StartContext;

static void worker_begin(NcEngineRunner *runner) {
    EnterCriticalSection(&runner->worker_lock);
    runner->worker_count++;
    LeaveCriticalSection(&runner->worker_lock);
}

static void worker_finish(NcEngineRunner *runner) {
    EnterCriticalSection(&runner->worker_lock);
    runner->worker_count--;
    if (runner->worker_count == 0) {
        WakeAllConditionVariable(&runner->worker_cv);
    }
    LeaveCriticalSection(&runner->worker_lock);
}

static unsigned __stdcall start_worker(void *parameter) {
    StartContext *context = (StartContext *)parameter;
    NcEngineRunner *runner = context->runner;
    NcErr error;
    NcEngineEvent event;
    bool ok;
    size_t index;
    memset(&event, 0, sizeof(event));
    event.kind = NC_ENGINE_EVENT_START_DONE;
    ok = nc_conditioner_start(runner->engine, &context->profile, NULL, NULL,
                              context->max_queue_delay_ms,
                              (const char *const *)context->names, context->name_count, &error);
    event.ok = ok;
    if (!ok) {
        snprintf(event.message, sizeof(event.message), "%s", error.msg);
    }
    nc_engine_events_send(runner->events, &event);
    nc_config_free_profile(&context->profile);
    for (index = 0; index < context->name_count; index++) {
        free(context->names[index]);
    }
    free(context->names);
    free(context);
    worker_finish(runner);
    return 0;
}

static unsigned __stdcall stop_worker(void *parameter) {
    NcEngineRunner *runner = (NcEngineRunner *)parameter;
    NcEngineEvent event;
    nc_conditioner_stop(runner->engine);
    memset(&event, 0, sizeof(event));
    event.kind = NC_ENGINE_EVENT_STOP_DONE;
    event.ok = true;
    nc_engine_events_send(runner->events, &event);
    worker_finish(runner);
    return 0;
}

NcEngineRunner *nc_engine_runner_new(HWND wake_hwnd, UINT wake_message,
                                     const NcEngineRunnerCallbacks *callbacks) {
    NcEngineRunner *runner = (NcEngineRunner *)calloc(1, sizeof(NcEngineRunner));
    if (!runner) {
        return NULL;
    }
    if (callbacks) {
        runner->callbacks = *callbacks;
    }
    InitializeCriticalSection(&runner->worker_lock);
    InitializeConditionVariable(&runner->worker_cv);
    runner->events = nc_engine_events_new(wake_hwnd, wake_message);
    if (!runner->events) {
        nc_engine_runner_free(runner);
        return NULL;
    }
    runner->engine = nc_conditioner_new(runner->events);
    if (!runner->engine) {
        nc_engine_runner_free(runner);
        return NULL;
    }
    return runner;
}

void nc_engine_runner_free(NcEngineRunner *runner) {
    if (!runner) {
        return;
    }
    EnterCriticalSection(&runner->worker_lock);
    while (runner->worker_count > 0) {
        SleepConditionVariableCS(&runner->worker_cv, &runner->worker_lock, INFINITE);
    }
    LeaveCriticalSection(&runner->worker_lock);
    if (runner->engine) {
        nc_conditioner_free(runner->engine);
    }
    if (runner->events) {
        nc_engine_events_free(runner->events);
    }
    DeleteCriticalSection(&runner->worker_lock);
    free(runner);
}

bool nc_engine_runner_is_running(const NcEngineRunner *runner) {
    return runner ? runner->running : false;
}

bool nc_engine_runner_is_busy(const NcEngineRunner *runner) {
    return runner ? runner->busy : false;
}

void nc_engine_runner_reschedule_auto_stop(NcEngineRunner *runner, int32_t max_run_minutes) {
    if (!runner) {
        return;
    }
    if (max_run_minutes > 0 && runner->has_run_started) {
        runner->auto_stop_ns = runner->run_started_ns + (uint64_t)max_run_minutes * 60ull * 1000000000ull;
        runner->has_auto_stop = true;
    } else {
        runner->has_auto_stop = false;
    }
}

bool nc_engine_runner_auto_stop_due(NcEngineRunner *runner) {
    if (!runner || !runner->running || !runner->has_auto_stop) {
        return false;
    }
    if (nc_monotonic_ns() >= runner->auto_stop_ns) {
        runner->has_auto_stop = false;
        return true;
    }
    return false;
}

void nc_engine_runner_stop_sync(NcEngineRunner *runner) {
    if (!runner) {
        return;
    }
    nc_conditioner_stop(runner->engine);
    runner->running = false;
    runner->busy = false;
    runner->has_auto_stop = false;
}

bool nc_engine_runner_start_async(NcEngineRunner *runner, const NcProfile *profile,
                                  const NcSettings *settings) {
    StartContext *context;
    HANDLE thread;
    size_t index;
    if (!runner || !profile || !settings || runner->busy || runner->running) {
        return false;
    }
    context = (StartContext *)calloc(1, sizeof(StartContext));
    if (!context) {
        return false;
    }
    context->runner = runner;
    if (!nc_config_copy_profile(profile, &context->profile)) {
        free(context);
        return false;
    }
    context->max_queue_delay_ms = settings->max_queue_delay_ms;
    context->name_count = settings->process_names.count;
    if (context->name_count > 0) {
        context->names = (char **)calloc(context->name_count, sizeof(char *));
        if (!context->names) {
            nc_config_free_profile(&context->profile);
            free(context);
            return false;
        }
        for (index = 0; index < context->name_count; index++) {
            context->names[index] = nc_strdup(settings->process_names.items[index]);
        }
    }
    runner->busy = true;
    if (runner->callbacks.on_state_changed) {
        runner->callbacks.on_state_changed(runner->callbacks.userdata);
    }
    worker_begin(runner);
    thread = (HANDLE)_beginthreadex(NULL, 0, start_worker, context, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return true;
    }
    worker_finish(runner);
    nc_config_free_profile(&context->profile);
    for (index = 0; index < context->name_count; index++) {
        free(context->names[index]);
    }
    free(context->names);
    free(context);
    runner->busy = false;
    if (runner->callbacks.on_state_changed) {
        runner->callbacks.on_state_changed(runner->callbacks.userdata);
    }
    return false;
}

bool nc_engine_runner_stop_async(NcEngineRunner *runner, bool silent) {
    HANDLE thread;
    if (!runner || !runner->running || runner->busy) {
        return false;
    }
    runner->busy = true;
    runner->stop_silent = silent;
    if (runner->callbacks.on_state_changed) {
        runner->callbacks.on_state_changed(runner->callbacks.userdata);
    }
    worker_begin(runner);
    thread = (HANDLE)_beginthreadex(NULL, 0, stop_worker, runner, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return true;
    }
    worker_finish(runner);
    runner->busy = false;
    if (runner->callbacks.on_state_changed) {
        runner->callbacks.on_state_changed(runner->callbacks.userdata);
    }
    return false;
}

void nc_engine_runner_stats(NcEngineRunner *runner, NcStatsSnapshot *uplink,
                            NcStatsSnapshot *downlink) {
    if (uplink) {
        memset(uplink, 0, sizeof(*uplink));
    }
    if (downlink) {
        memset(downlink, 0, sizeof(*downlink));
    }
    if (!runner || !runner->running) {
        return;
    }
    if (uplink) {
        nc_conditioner_uplink_stats(runner->engine, uplink);
    }
    if (downlink) {
        nc_conditioner_downlink_stats(runner->engine, downlink);
    }
}

void nc_engine_runner_drain(NcEngineRunner *runner, int32_t max_run_minutes) {
    NcEngineEvent event;
    if (!runner || !runner->events) {
        return;
    }
    while (nc_engine_events_pop(runner->events, &event)) {
        switch (event.kind) {
        case NC_ENGINE_EVENT_START_DONE:
            if (event.ok) {
                runner->running = true;
                runner->busy = false;
                runner->run_started_ns = nc_monotonic_ns();
                runner->has_run_started = true;
                nc_engine_runner_reschedule_auto_stop(runner, max_run_minutes);
                nc_conditioner_reset_stats(runner->engine);
                if (runner->callbacks.on_start_succeeded) {
                    runner->callbacks.on_start_succeeded(runner->callbacks.userdata);
                }
            } else {
                nc_conditioner_stop(runner->engine);
                runner->running = false;
                runner->busy = false;
                if (runner->callbacks.on_start_failed) {
                    runner->callbacks.on_start_failed(runner->callbacks.userdata, event.message);
                }
            }
            break;
        case NC_ENGINE_EVENT_STOP_DONE: {
            bool silent = runner->stop_silent;
            runner->running = false;
            runner->busy = false;
            runner->has_auto_stop = false;
            runner->stop_silent = false;
            if (runner->callbacks.on_stop_finished) {
                runner->callbacks.on_stop_finished(runner->callbacks.userdata, silent);
            }
            break;
        }
        case NC_ENGINE_EVENT_FAULT:
            if (runner->running) {
                nc_engine_runner_stop_sync(runner);
                if (runner->callbacks.on_fault) {
                    runner->callbacks.on_fault(runner->callbacks.userdata, event.message);
                }
            }
            break;
        default:
            break;
        }
    }
}
