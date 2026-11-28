#include "ui/ui_host.h"

#include "core/win.h"

int nc_ui_host_message(const NcUiHost *host, const char *text, const char *title, UINT flags) {
    HWND owner = host ? host->owner : NULL;
    return nc_message_box(owner, text, title, flags);
}

LRESULT nc_ui_paint_surface(HWND hwnd) {
    PAINTSTRUCT paint;
    HDC hdc = nc_begin_paint(hwnd, &paint);
    RECT rect = nc_client_rect(hwnd);
    nc_fill_rect(hdc, &rect, NC_COLOR_SURFACE);
    nc_end_paint(hwnd, &paint);
    return 0;
}

LRESULT nc_ui_draw_item(const NcWidgets *widgets, const NcFonts *fonts, LPARAM lparam) {
    if (widgets && fonts && lparam) {
        DRAWITEMSTRUCT *draw = (DRAWITEMSTRUCT *)lparam;
        nc_widget_draw_item(draw, widgets, fonts);
        return 1;
    }
    return 0;
}

bool nc_ui_button_colors(NcWidgets *widgets, HWND hwnd, uint32_t *background, uint32_t *foreground) {
    NcWidget *widget;
    if (!widgets || !hwnd) {
        return false;
    }
    widget = nc_widget_find(widgets, hwnd);
    if (!widget || widget->kind != NC_WIDGET_BUTTON) {
        return false;
    }
    if (background) {
        *background = nc_is_window_enabled(hwnd) ? widget->button_bg : widget->disabled_bg;
    }
    if (foreground) {
        *foreground = widget->button_fg;
    }
    return true;
}

bool nc_ui_handle_chrome(NcWidgets *widgets, const NcFonts *fonts, HBRUSH white_brush, HWND hwnd,
                         UINT message, WPARAM wparam, LPARAM lparam, uint32_t static_bk_color,
                         LRESULT *result) {
    switch (message) {
    case WM_ERASEBKGND:
        *result = 1;
        return true;
    case WM_PAINT:
        *result = nc_ui_paint_surface(hwnd);
        return true;
    case WM_DRAWITEM:
        *result = nc_ui_draw_item(widgets, fonts, lparam);
        return true;
    case WM_CTLCOLORSTATIC:
        nc_set_bk_color((HDC)wparam, static_bk_color);
        nc_set_text_color((HDC)wparam, NC_COLOR_TEXT_DARK);
        *result = (LRESULT)white_brush;
        return true;
    case WM_CTLCOLOREDIT:
        nc_set_bk_color((HDC)wparam, NC_COLOR_CARD);
        nc_set_text_color((HDC)wparam, NC_COLOR_TEXT_DARK);
        *result = (LRESULT)white_brush;
        return true;
    case WM_CTLCOLORBTN: {
        uint32_t background;
        uint32_t foreground;
        if (nc_ui_button_colors(widgets, (HWND)lparam, &background, &foreground)) {
            nc_set_dc_brush_color((HDC)wparam, background);
            nc_set_text_color((HDC)wparam, foreground);
            *result = (LRESULT)nc_dc_brush();
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}
