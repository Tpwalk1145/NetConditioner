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

int nc_app_run(void);

NcWidgets *nc_app_widgets(NcApp *app);
NcFonts *nc_app_fonts(NcApp *app);
HWND nc_app_main_hwnd(NcApp *app);
double nc_app_scale(NcApp *app);
HBRUSH nc_app_white_brush(NcApp *app);
NcConfig *nc_app_config(NcApp *app);
int nc_app_show_message(NcApp *app, const char *text, const char *title, UINT flags);
void nc_app_cleanup_dialog_widgets(NcApp *app, HWND hwnd);
LRESULT nc_app_draw_item(NcApp *app, LPARAM lparam);
LRESULT nc_app_paint_surface(NcApp *app, HWND hwnd);
int nc_app_button_colors(NcApp *app, HWND hwnd, uint32_t *background, uint32_t *foreground);
void nc_app_save_config(NcApp *app);
void nc_app_settings_hotkey_pressed(NcApp *app, uint32_t modifiers, int32_t key);
void nc_app_settings_request_cancel(NcApp *app);
void nc_app_settings_request_confirm(NcApp *app);
LRESULT nc_app_dialog_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);