#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/str.h"

#define NC_WM_APP_EVENTS (WM_APP + 1)
#define NC_WM_APP_TRAY (WM_APP + 2)

#define NC_STYLE_MAIN (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN)
#define NC_STYLE_DIALOG (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)

HINSTANCE nc_hinstance(void);
bool nc_register_class(const wchar_t *name, WNDPROC proc);
HWND nc_create_window(const wchar_t *class_name, const wchar_t *title, DWORD style,
                      DWORD ex_style, HWND parent, HMENU menu, void *param);
HWND nc_create_child(HWND parent, const wchar_t *class_name, const wchar_t *text, DWORD style,
                     DWORD ex_style, int x, int y, int width, int height, int id);
void nc_set_window_client_size(HWND hwnd, int width, int height);
void nc_center_on_screen(HWND hwnd);
UINT nc_dpi_of(HWND hwnd);
double nc_fit_dialog_scale(HWND owner, int base_width, int base_height, double preferred);
void nc_center_on_owner(HWND hwnd, HWND owner);
void nc_run_modal(HWND owner, HWND dialog);
bool nc_post(HWND hwnd, UINT message);
LRESULT nc_send(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
void nc_set_text(HWND hwnd, const char *utf8);
char *nc_get_text(HWND hwnd);
int nc_message_box(HWND owner, const char *text, const char *title, UINT flags);
void nc_enable(HWND hwnd, bool enabled);
void nc_show(HWND hwnd, int command);
void nc_destroy(HWND hwnd);
void nc_invalidate(HWND hwnd);
void nc_set_font(HWND hwnd, HFONT font);
void nc_set_timer(HWND hwnd, UINT_PTR id, UINT milliseconds);
void nc_kill_timer(HWND hwnd, UINT_PTR id);
int nc_message_loop(void);
void nc_set_foreground(HWND hwnd);
void nc_set_focus(HWND hwnd);
void nc_fill_rect(HDC hdc, const RECT *rect, uint32_t color);
void nc_frame_rect(HDC hdc, const RECT *rect, uint32_t color);
UINT nc_system_dpi(void);
void nc_set_window_pos(HWND hwnd, int x, int y, int width, int height);
void nc_draw_text(HDC hdc, const char *text, RECT *rect, HFONT font, uint32_t color, UINT format);
HFONT nc_create_font(const wchar_t *family, UINT size_x10, UINT dpi, bool bold, bool underline);
HDC nc_begin_paint(HWND hwnd, PAINTSTRUCT *paint);
void nc_end_paint(HWND hwnd, PAINTSTRUCT *paint);
RECT nc_client_rect(HWND hwnd);
HBRUSH nc_create_white_brush(void);
void nc_set_bk_color(HDC hdc, uint32_t color);
void nc_set_text_color(HDC hdc, uint32_t color);
int nc_last_error_code(void);
void nc_delete_object(HGDIOBJ object);
HICON nc_create_circle_icon(uint32_t color, int size);
void nc_destroy_icon(HICON icon);
void nc_write_fixed(wchar_t *buffer, size_t cap, const char *utf8);
bool nc_is_elevated(void);
HANDLE nc_create_single_instance_mutex(void);
void nc_init_common_controls(void);
void nc_set_dpi_awareness(void);
bool nc_is_remote_session(void);
void nc_set_dc_brush_color(HDC hdc, uint32_t color);
HBRUSH nc_dc_brush(void);
bool nc_is_window_enabled(HWND hwnd);
void nc_set_cue_banner(HWND edit, const char *text);
bool nc_key_down(int vk);
bool nc_register_hotkey(HWND hwnd, int id, uint32_t modifiers, int key);
void nc_unregister_hotkey(HWND hwnd, int id);
