#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>

#include "config.h"
#include "ui/widgets.h"

typedef struct NcApp NcApp;
typedef struct NcProcessSelector NcProcessSelector;

NcProcessSelector *nc_process_selector_create(NcApp *app);
void nc_process_selector_free(NcProcessSelector *selector);
HWND nc_process_selector_hwnd(const NcProcessSelector *selector);
const NcFonts *nc_process_selector_fonts(const NcProcessSelector *selector);
bool nc_process_selector_closed(const NcProcessSelector *selector);
bool nc_process_selector_message(NcProcessSelector *selector, NcApp *app, UINT message,
                                 WPARAM wparam, LPARAM lparam, LRESULT *result);
bool nc_process_selector_take_outcome(NcProcessSelector *selector, NcStringList *out);
