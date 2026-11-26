#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "ui/widgets.h"

typedef struct NcApp NcApp;
typedef struct NcSettingsDialog NcSettingsDialog;

typedef struct {
    uint32_t modifiers;
    int32_t key;
    int32_t max_run_minutes;
    int32_t max_queue_delay_ms;
    bool auto_update;
    char *update_feed_url;
} NcSettingsOutcome;

NcSettingsDialog *nc_settings_dialog_create(NcApp *app);
void nc_settings_dialog_free(NcSettingsDialog *dialog);
HWND nc_settings_dialog_hwnd(const NcSettingsDialog *dialog);
const NcFonts *nc_settings_dialog_fonts(const NcSettingsDialog *dialog);
bool nc_settings_dialog_closed(const NcSettingsDialog *dialog);
bool nc_settings_dialog_message(NcSettingsDialog *dialog, NcApp *app, UINT message,
                                WPARAM wparam, LPARAM lparam, LRESULT *result);
bool nc_settings_dialog_take_outcome(NcSettingsDialog *dialog, NcSettingsOutcome *out);
void nc_settings_set_candidate(NcSettingsDialog *dialog, NcApp *app, uint32_t modifiers,
                               int32_t key);
void nc_settings_cancel(NcSettingsDialog *dialog, NcApp *app);
void nc_settings_confirm(NcSettingsDialog *dialog, NcApp *app);
