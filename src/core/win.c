#include "core/win.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HINSTANCE nc_hinstance(void) {
    return GetModuleHandleW(NULL);
}

bool nc_register_class(const wchar_t *name, WNDPROC proc) {
    WNDCLASSEXW window_class;
    ATOM atom;
    if (!name || !proc) {
        return false;
    }
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = proc;
    window_class.hInstance = nc_hinstance();
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = NULL;
    window_class.lpszClassName = name;
    atom = RegisterClassExW(&window_class);
    return atom != 0;
}

HWND nc_create_window(const wchar_t *class_name, const wchar_t *title, DWORD style, DWORD ex_style,
                      HWND parent, HMENU menu, void *param) {
    return CreateWindowExW(ex_style, class_name, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                           CW_USEDEFAULT, CW_USEDEFAULT, parent, menu, nc_hinstance(), param);
}

HWND nc_create_child(HWND parent, const wchar_t *class_name, const wchar_t *text, DWORD style,
                     DWORD ex_style, int x, int y, int width, int height, int id) {
    return CreateWindowExW(ex_style, class_name, text, style, x, y, width, height, parent,
                           (HMENU)(INT_PTR)id, nc_hinstance(), NULL);
}

void nc_set_window_client_size(HWND hwnd, int width, int height) {
    DWORD style;
    DWORD ex_style;
    RECT rect;
    if (!hwnd) {
        return;
    }
    style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    ex_style = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, nc_dpi_of(hwnd));
    SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void nc_center_on_screen(HWND hwnd) {
    HMONITOR monitor;
    MONITORINFO info;
    RECT rect;
    int width;
    int height;
    if (!hwnd) {
        return;
    }
    monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }
    GetWindowRect(hwnd, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    SetWindowPos(hwnd, NULL,
                 info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2,
                 info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - height) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

UINT nc_dpi_of(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    return dpi ? dpi : 96;
}

double nc_fit_dialog_scale(HWND owner, int base_width, int base_height, double preferred) {
    HMONITOR monitor;
    MONITORINFO info;
    double width;
    double height;
    double fit;
    monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return preferred;
    }
    width = (double)(info.rcWork.right - info.rcWork.left - 32);
    height = (double)(info.rcWork.bottom - info.rcWork.top - 32);
    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }
    fit = width / (base_width > 0 ? base_width : 1);
    {
        double vertical = height / (base_height > 0 ? base_height : 1);
        if (vertical < fit) {
            fit = vertical;
        }
    }
    if (fit > preferred) {
        fit = preferred;
    }
    if (fit < 0.5) {
        fit = 0.5;
    }
    return fit;
}

void nc_center_on_owner(HWND hwnd, HWND owner) {
    RECT owner_rect;
    RECT rect;
    int width;
    int height;
    if (!hwnd) {
        return;
    }
    if (!owner || !GetWindowRect(owner, &owner_rect)) {
        nc_center_on_screen(hwnd);
        return;
    }
    GetWindowRect(hwnd, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    SetWindowPos(hwnd, NULL,
                 owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2,
                 owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void nc_run_modal(HWND owner, HWND dialog) {
    MSG message;
    if (!dialog) {
        return;
    }
    if (owner) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
    for (;;) {
        int result;
        if (!IsWindow(dialog)) {
            break;
        }
        result = (int)GetMessageW(&message, NULL, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                PostQuitMessage(0);
            }
            break;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
}

bool nc_post(HWND hwnd, UINT message) {
    return PostMessageW(hwnd, message, 0, 0) != 0;
}

LRESULT nc_send(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return SendMessageW(hwnd, message, wparam, lparam);
}

void nc_set_text(HWND hwnd, const char *utf8) {
    wchar_t *wide = nc_wide_dup(utf8 ? utf8 : "");
    if (!wide) {
        return;
    }
    SetWindowTextW(hwnd, wide);
    free(wide);
}

char *nc_get_text(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    wchar_t *wide;
    char *utf8;
    if (length < 0) {
        return nc_strdup("");
    }
    wide = (wchar_t *)malloc(((size_t)length + 1) * sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    GetWindowTextW(hwnd, wide, length + 1);
    wide[length] = 0;
    utf8 = nc_utf8_dup(wide);
    free(wide);
    return utf8;
}

int nc_message_box(HWND owner, const char *text, const char *title, UINT flags) {
    wchar_t *wide_text = nc_wide_dup(text ? text : "");
    wchar_t *wide_title = nc_wide_dup(title ? title : "NetConditioner");
    int result;
    if (!wide_text || !wide_title) {
        free(wide_text);
        free(wide_title);
        return 0;
    }
    result = MessageBoxW(owner, wide_text, wide_title, flags);
    free(wide_text);
    free(wide_title);
    return result;
}

void nc_enable(HWND hwnd, bool enabled) {
    EnableWindow(hwnd, enabled ? TRUE : FALSE);
}

void nc_show(HWND hwnd, int command) {
    ShowWindow(hwnd, command);
}

void nc_destroy(HWND hwnd) {
    if (hwnd) {
        DestroyWindow(hwnd);
    }
}

void nc_invalidate(HWND hwnd) {
    InvalidateRect(hwnd, NULL, TRUE);
}

void nc_set_font(HWND hwnd, HFONT font) {
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    }
}

void nc_set_timer(HWND hwnd, UINT_PTR id, UINT milliseconds) {
    SetTimer(hwnd, id, milliseconds, NULL);
}

void nc_kill_timer(HWND hwnd, UINT_PTR id) {
    KillTimer(hwnd, id);
}

int nc_message_loop(void) {
    MSG message;
    memset(&message, 0, sizeof(message));
    for (;;) {
        int result = (int)GetMessageW(&message, NULL, 0, 0);
        if (result <= 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}

void nc_set_foreground(HWND hwnd) {
    SetForegroundWindow(hwnd);
}

void nc_set_focus(HWND hwnd) {
    SetFocus(hwnd);
}

void nc_fill_rect(HDC hdc, const RECT *rect, uint32_t color) {
    HBRUSH brush = CreateSolidBrush((COLORREF)color);
    FillRect(hdc, rect, brush);
    DeleteObject(brush);
}

void nc_frame_rect(HDC hdc, const RECT *rect, uint32_t color) {
    HBRUSH brush = CreateSolidBrush((COLORREF)color);
    FrameRect(hdc, rect, brush);
    DeleteObject(brush);
}

UINT nc_system_dpi(void) {
    return GetDpiForSystem();
}

void nc_set_window_pos(HWND hwnd, int x, int y, int width, int height) {
    SetWindowPos(hwnd, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void nc_draw_text(HDC hdc, const char *text, RECT *rect, HFONT font, uint32_t color, UINT format) {
    wchar_t *wide = nc_wide_dup(text ? text : "");
    HGDIOBJ previous;
    if (!wide) {
        return;
    }
    previous = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, (COLORREF)color);
    DrawTextW(hdc, wide, -1, rect, format);
    SelectObject(hdc, previous);
    free(wide);
}

HFONT nc_create_font(const wchar_t *family, UINT size_x10, UINT dpi, bool bold, bool underline) {
    int height = -(int)(((double)size_x10 * (double)dpi / 720.0) + 0.5);
    return CreateFontW(height, 0, 0, 0, bold ? 700 : 400, 0, underline ? 1 : 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, family);
}

HDC nc_begin_paint(HWND hwnd, PAINTSTRUCT *paint) {
    return BeginPaint(hwnd, paint);
}

void nc_end_paint(HWND hwnd, PAINTSTRUCT *paint) {
    EndPaint(hwnd, paint);
}

RECT nc_client_rect(HWND hwnd) {
    RECT rect;
    rect.left = 0;
    rect.top = 0;
    rect.right = 0;
    rect.bottom = 0;
    GetClientRect(hwnd, &rect);
    return rect;
}

HBRUSH nc_create_white_brush(void) {
    return CreateSolidBrush(RGB(255, 255, 255));
}

void nc_set_bk_color(HDC hdc, uint32_t color) {
    SetBkColor(hdc, (COLORREF)color);
}

void nc_set_text_color(HDC hdc, uint32_t color) {
    SetTextColor(hdc, (COLORREF)color);
}

int nc_last_error_code(void) {
    return (int)GetLastError();
}

void nc_delete_object(HGDIOBJ object) {
    if (object) {
        DeleteObject(object);
    }
}

HICON nc_create_circle_icon(uint32_t color, int size) {
    HDC hdc;
    BITMAPINFO info;
    void *bits = NULL;
    HBITMAP color_bitmap;
    HGDIOBJ previous;
    int margin;
    int ring;
    HBRUSH brush;
    HPEN pen;
    HGDIOBJ previous_brush;
    HGDIOBJ previous_pen;
    unsigned char *pixels;
    int x;
    int y;
    size_t stride;
    unsigned char *mask_bits;
    HBITMAP mask;
    ICONINFO icon_info;
    HICON icon;
    if (size <= 0) {
        return NULL;
    }
    hdc = CreateCompatibleDC(NULL);
    if (!hdc) {
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    color_bitmap = CreateDIBSection(hdc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!color_bitmap || !bits) {
        DeleteDC(hdc);
        return NULL;
    }
    previous = SelectObject(hdc, color_bitmap);
    {
        RECT rect;
        rect.left = 0;
        rect.top = 0;
        rect.right = size;
        rect.bottom = size;
        nc_fill_rect(hdc, &rect, 0);
    }
    margin = (int)((double)size * 0.09 + 0.5);
    ring = (int)((double)size * 0.09 + 0.5);
    brush = CreateSolidBrush((COLORREF)color);
    pen = CreatePen(PS_SOLID, ring > 0 ? ring : 1, RGB(255, 255, 255));
    previous_brush = SelectObject(hdc, brush);
    previous_pen = SelectObject(hdc, pen);
    Ellipse(hdc, margin, margin, size - margin, size - margin);
    SelectObject(hdc, previous_brush);
    SelectObject(hdc, previous_pen);
    DeleteObject(brush);
    DeleteObject(pen);
    SelectObject(hdc, previous);
    DeleteDC(hdc);
    pixels = (unsigned char *)bits;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            unsigned char *pixel = pixels + ((size_t)y * (size_t)size + (size_t)x) * 4;
            if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
                pixel[3] = 255;
            }
        }
    }
    stride = (size_t)(((size + 15) / 16) * 2);
    mask_bits = (unsigned char *)calloc(stride * (size_t)size, 1);
    if (!mask_bits) {
        DeleteObject(color_bitmap);
        return NULL;
    }
    mask = CreateBitmap(size, size, 1, 1, mask_bits);
    free(mask_bits);
    memset(&icon_info, 0, sizeof(icon_info));
    icon_info.fIcon = 1;
    icon_info.hbmMask = mask;
    icon_info.hbmColor = color_bitmap;
    icon = CreateIconIndirect(&icon_info);
    DeleteObject(mask);
    DeleteObject(color_bitmap);
    return icon;
}

void nc_destroy_icon(HICON icon) {
    if (icon) {
        DestroyIcon(icon);
    }
}

void nc_write_fixed(wchar_t *buffer, size_t cap, const char *utf8) {
    wchar_t *wide;
    size_t length;
    size_t copy;
    if (!buffer || cap == 0) {
        return;
    }
    memset(buffer, 0, cap * sizeof(wchar_t));
    wide = nc_wide_dup(utf8 ? utf8 : "");
    if (!wide) {
        return;
    }
    length = wcslen(wide);
    copy = length < cap - 1 ? length : cap - 1;
    memcpy(buffer, wide, copy * sizeof(wchar_t));
    buffer[copy] = 0;
    free(wide);
}

bool nc_is_elevated(void) {
    SID_IDENTIFIER_AUTHORITY authority = {SECURITY_NT_AUTHORITY};
    PSID administrators = NULL;
    BOOL member = FALSE;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators)) {
        return false;
    }
    CheckTokenMembership(NULL, administrators, &member);
    FreeSid(administrators);
    return member != 0;
}

HANDLE nc_create_single_instance_mutex(void) {
    HANDLE handle = CreateMutexW(NULL, TRUE, L"Global\\NetConditioner.SingleInstance");
    if (!handle) {
        return INVALID_HANDLE_VALUE;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return NULL;
    }
    return handle;
}

void nc_init_common_controls(void) {
    INITCOMMONCONTROLSEX info;
    info.dwSize = sizeof(info);
    info.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&info);
}

void nc_set_dpi_awareness(void) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

bool nc_is_remote_session(void) {
    return GetSystemMetrics(SM_REMOTESESSION) != 0;
}

void nc_set_dc_brush_color(HDC hdc, uint32_t color) {
    SetDCBrushColor(hdc, (COLORREF)color);
}

HBRUSH nc_dc_brush(void) {
    return (HBRUSH)GetStockObject(DC_BRUSH);
}

bool nc_is_window_enabled(HWND hwnd) {
    return ((DWORD)GetWindowLongW(hwnd, GWL_STYLE) & WS_DISABLED) == 0;
}

void nc_set_cue_banner(HWND edit, const char *text) {
    wchar_t *wide = nc_wide_dup(text ? text : "");
    if (!wide) {
        return;
    }
    SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)wide);
    free(wide);
}

bool nc_key_down(int vk) {
    return (GetKeyState(vk) & 0x8000) != 0;
}

bool nc_register_hotkey(HWND hwnd, int id, uint32_t modifiers, int key) {
    return RegisterHotKey(hwnd, id, modifiers | MOD_NOREPEAT, (UINT)key) != 0;
}

void nc_unregister_hotkey(HWND hwnd, int id) {
    UnregisterHotKey(hwnd, id);
}
