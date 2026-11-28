#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/widgets.h"

#define NC_MAIN_CLIENT_WIDTH 560
#define NC_MAIN_CLIENT_HEIGHT 574

typedef struct {
    HWND status_card;
    HWND stats_card;
    HWND profile_combo;
    HWND new_button;
    HWND edit_button;
    HWND delete_button;
    HWND process_box;
    HWND process_button;
    HWND toggle_button;
    HWND hotkey_label;
    HWND settings_link;
    HWND uplink_stats;
    HWND downlink_stats;
    HWND status_dot;
    HWND status_label;
    HWND status_detail;
    HWND minimize_button;
    HWND exit_button;
} NcMainControls;

typedef struct {
    HWND hwnd;
    UINT dpi;
    double scale;
    NcMainControls controls;
} NcMainWindow;

HWND nc_main_window_create(void *param);
void nc_main_window_init(NcMainWindow *window, HWND hwnd);
void nc_main_window_build(NcMainWindow *window, NcWidgets *widgets, const NcFonts *fonts);
void nc_main_window_apply_fonts(const NcMainWindow *window, const NcFonts *fonts);
