#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>

typedef enum {
    NC_TRAY_COMMAND_NONE = 0,
    NC_TRAY_COMMAND_SHOW,
    NC_TRAY_COMMAND_TOGGLE,
    NC_TRAY_COMMAND_SETTINGS,
    NC_TRAY_COMMAND_EXIT
} NcTrayCommand;

typedef struct {
    NOTIFYICONDATAW data;
    HICON normal;
    HICON active;
    bool added;
} NcTray;

void nc_tray_init(NcTray *tray, HWND hwnd);
void nc_tray_destroy(NcTray *tray);
void nc_tray_restore(NcTray *tray);
void nc_tray_set_running(NcTray *tray, bool running);
void nc_tray_balloon(NcTray *tray, const char *title, const char *text, UINT flags);
NcTrayCommand nc_tray_handle_message(NcTray *tray, LPARAM lparam, bool running);