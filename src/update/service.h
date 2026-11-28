#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>

#include "config/config.h"

/* Decoupled host callbacks so update/service has zero dependency on app or ui. */
typedef struct {
    void *userdata;
    bool (*is_idle)(void *userdata);
    void (*save_config)(void *userdata);
    void (*stop_engine_sync)(void *userdata);
    void (*exit_for_update)(void *userdata);
} NcUpdateHost;

typedef struct NcUpdateService NcUpdateService;

NcUpdateService *nc_update_service_new(HWND notify_hwnd, UINT notify_message,
                                       const NcUpdateHost *host);
void nc_update_service_free(NcUpdateService *service);
void nc_update_service_detach_window(NcUpdateService *service);
void nc_update_service_on_settings_changed(NcUpdateService *service, NcSettings *settings);
void nc_update_service_maybe_tick(NcUpdateService *service, NcSettings *settings);
void nc_update_service_try_install(NcUpdateService *service, NcSettings *settings);
void nc_update_service_drain(NcUpdateService *service, NcSettings *settings);
