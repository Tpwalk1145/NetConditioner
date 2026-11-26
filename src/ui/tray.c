#include "tray.h"

#include <shellapi.h>
#include <string.h>

#include "widgets.h"
#include "win.h"

#define NC_TRAY_ID 1

void nc_tray_init(NcTray *tray, HWND hwnd) {
    if (!tray || !hwnd) {
        return;
    }
    memset(tray, 0, sizeof(*tray));
    tray->normal = nc_create_circle_icon(RGB(76, 175, 80), 32);
    tray->active = nc_create_circle_icon(NC_COLOR_ORANGE, 32);
    tray->data.cbSize = sizeof(tray->data);
    tray->data.hWnd = hwnd;
    tray->data.uID = NC_TRAY_ID;
    tray->data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray->data.uCallbackMessage = NC_WM_APP_TRAY;
    tray->data.hIcon = tray->normal;
    nc_write_fixed(tray->data.szTip, sizeof(tray->data.szTip) / sizeof(tray->data.szTip[0]),
                   "弱网模拟器 - 未连接");
    tray->added = Shell_NotifyIconW(NIM_ADD, &tray->data) != 0;
}

void nc_tray_restore(NcTray *tray) {
    if (!tray) {
        return;
    }
    if (tray->added && Shell_NotifyIconW(NIM_MODIFY, &tray->data) != 0) {
        return;
    }
    if (tray->added) {
        Shell_NotifyIconW(NIM_DELETE, &tray->data);
    }
    tray->added = Shell_NotifyIconW(NIM_ADD, &tray->data) != 0;
}

void nc_tray_set_running(NcTray *tray, bool running) {
    if (!tray) {
        return;
    }
    tray->data.hIcon = running ? tray->active : tray->normal;
    nc_write_fixed(tray->data.szTip, sizeof(tray->data.szTip) / sizeof(tray->data.szTip[0]),
                   running ? "弱网模拟器 - 已连接" : "弱网模拟器 - 未连接");
    tray->data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    if (!tray->added) {
        nc_tray_restore(tray);
        return;
    }
    Shell_NotifyIconW(NIM_MODIFY, &tray->data);
}

void nc_tray_balloon(NcTray *tray, const char *title, const char *text, UINT flags) {
    NOTIFYICONDATAW data;
    if (!tray) {
        return;
    }
    data = tray->data;
    data.uFlags |= NIF_INFO;
    data.dwInfoFlags = flags;
    nc_write_fixed(data.szInfoTitle, sizeof(data.szInfoTitle) / sizeof(data.szInfoTitle[0]),
                   title ? title : "");
    nc_write_fixed(data.szInfo, sizeof(data.szInfo) / sizeof(data.szInfo[0]), text ? text : "");
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

static NcTrayCommand show_menu(NcTray *tray, bool running) {
    HMENU menu = CreatePopupMenu();
    POINT cursor;
    wchar_t *show_text;
    wchar_t *toggle_text;
    wchar_t *settings_text;
    wchar_t *exit_text;
    DWORD command;
    if (!menu) {
        return NC_TRAY_COMMAND_NONE;
    }
    show_text = nc_wide_dup("显示主窗口");
    toggle_text = nc_wide_dup(running ? "停止并恢复" : "启动弱网");
    settings_text = nc_wide_dup("设置");
    exit_text = nc_wide_dup("退出");
    if (!show_text || !toggle_text || !settings_text || !exit_text) {
        free(show_text);
        free(toggle_text);
        free(settings_text);
        free(exit_text);
        DestroyMenu(menu);
        return NC_TRAY_COMMAND_NONE;
    }
    AppendMenuW(menu, MF_STRING, 1, show_text);
    AppendMenuW(menu, MF_STRING, 2, toggle_text);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 3, settings_text);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 4, exit_text);
    nc_set_foreground(tray->data.hWnd);
    GetCursorPos(&cursor);
    command = (DWORD)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    cursor.x, cursor.y, 0, tray->data.hWnd, NULL);
    PostMessageW(tray->data.hWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    free(show_text);
    free(toggle_text);
    free(settings_text);
    free(exit_text);
    switch (command) {
    case 1: return NC_TRAY_COMMAND_SHOW;
    case 2: return NC_TRAY_COMMAND_TOGGLE;
    case 3: return NC_TRAY_COMMAND_SETTINGS;
    case 4: return NC_TRAY_COMMAND_EXIT;
    default: return NC_TRAY_COMMAND_NONE;
    }
}

NcTrayCommand nc_tray_handle_message(NcTray *tray, LPARAM lparam, bool running) {
    UINT message = (UINT)lparam;
    if (!tray) {
        return NC_TRAY_COMMAND_NONE;
    }
    if (message == WM_LBUTTONDBLCLK || message == WM_LBUTTONUP) {
        return NC_TRAY_COMMAND_SHOW;
    }
    if (message == WM_RBUTTONUP) {
        return show_menu(tray, running);
    }
    return NC_TRAY_COMMAND_NONE;
}

void nc_tray_destroy(NcTray *tray) {
    if (!tray) {
        return;
    }
    if (tray->added) {
        Shell_NotifyIconW(NIM_DELETE, &tray->data);
        tray->added = false;
    }
    nc_destroy_icon(tray->normal);
    nc_destroy_icon(tray->active);
    tray->normal = NULL;
    tray->active = NULL;
}
