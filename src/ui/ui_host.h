#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "ui/widgets.h"

/* Decoupled UI host context passed to modal dialogs (zero dependency on app.h). */
typedef struct {
    HWND owner;
    double scale;
    HBRUSH white_brush;
} NcUiHost;

int nc_ui_host_message(const NcUiHost *host, const char *text, const char *title, UINT flags);
LRESULT nc_ui_paint_surface(HWND hwnd);
LRESULT nc_ui_draw_item(const NcWidgets *widgets, const NcFonts *fonts, LPARAM lparam);
bool nc_ui_button_colors(NcWidgets *widgets, HWND hwnd, uint32_t *background, uint32_t *foreground);
bool nc_ui_handle_chrome(NcWidgets *widgets, const NcFonts *fonts, HBRUSH white_brush, HWND hwnd,
                         UINT message, WPARAM wparam, LPARAM lparam, uint32_t static_bk_color,
                         LRESULT *result);
