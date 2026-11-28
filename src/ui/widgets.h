#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/win.h"

#define NC_COLOR_ACCENT 0x00C06515u
#define NC_COLOR_SURFACE 0x00FCF7F3u
#define NC_COLOR_CARD 0x00FFFFFFu
#define NC_COLOR_TEXT_DARK 0x00242120u
#define NC_COLOR_TEXT_MUTED 0x00706B68u
#define NC_COLOR_GREEN 0x00327D2Eu
#define NC_COLOR_RED 0x002828C6u
#define NC_COLOR_ORANGE 0x00006CEFu
#define NC_COLOR_BORDER 0x00CFCBC8u
#define NC_COLOR_TEXT_BUTTON 0x0043403Cu
#define NC_COLOR_SUBTITLE_TEXT 0x00FBDEBBu
#define NC_COLOR_WARNING_TEXT 0x0000577Eu
#define NC_COLOR_DANGER_BG 0x00EEEBFFu
#define NC_COLOR_DANGER_TEXT 0x001C1CB7u
#define NC_COLOR_DANGER_BORDER 0x009A9AEFu
#define NC_COLOR_GRAY 0x009E9E9Eu

typedef enum {
    NC_ALIGN_LEFT = 0,
    NC_ALIGN_CENTER
} NcAlign;

typedef enum {
    NC_FONT_HEADER = 0,
    NC_FONT_SUBTITLE,
    NC_FONT_STATUS,
    NC_FONT_DOT,
    NC_FONT_BODY,
    NC_FONT_BODY_BOLD,
    NC_FONT_SMALL,
    NC_FONT_SMALL85,
    NC_FONT_TITLE,
    NC_FONT_TOGGLE,
    NC_FONT_HOTKEY,
    NC_FONT_LINK,
    NC_FONT_LINK85,
    NC_FONT_ROLE_COUNT
} NcFontRole;

typedef struct {
    HFONT fonts[NC_FONT_ROLE_COUNT];
} NcFonts;

void nc_fonts_init(NcFonts *fonts, UINT dpi);
void nc_fonts_destroy(NcFonts *fonts);
HFONT nc_fonts_get(const NcFonts *fonts, NcFontRole role);

typedef enum {
    NC_WIDGET_PANEL = 0,
    NC_WIDGET_TEXT,
    NC_WIDGET_BUTTON
} NcWidgetKind;

typedef struct {
    HWND hwnd;
    NcWidgetKind kind;
    char *text;
    NcFontRole font;
    uint32_t color;
    uint32_t background;
    NcAlign align;
    bool multiline;
    bool vcenter;
    bool has_border;
    uint32_t border;
    uint32_t button_bg;
    uint32_t button_fg;
    uint32_t disabled_bg;
} NcWidget;

typedef struct {
    NcWidget *items;
    size_t count;
    size_t cap;
} NcWidgets;

void nc_widgets_init(NcWidgets *widgets);
void nc_widgets_free(NcWidgets *widgets);
void nc_widgets_clear(NcWidgets *widgets);
NcWidget *nc_widget_find(NcWidgets *widgets, HWND hwnd);
void nc_widgets_remove_window(NcWidgets *widgets, HWND window);

HWND nc_widget_create_panel(NcWidgets *widgets, HWND parent, int id, int x, int y, int width,
                            int height, uint32_t background, bool has_border, uint32_t border);
HWND nc_widget_create_text(NcWidgets *widgets, HWND parent, int id, const char *text, int x, int y,
                           int width, int height, NcFontRole font, uint32_t color,
                           uint32_t background, NcAlign align, bool multiline, bool vcenter);
HWND nc_widget_create_button(NcWidgets *widgets, HWND parent, int id, const char *text, int x,
                             int y, int width, int height, uint32_t background, uint32_t foreground,
                             NcFontRole font, uint32_t disabled_background, bool has_border,
                             uint32_t border, bool link);
HWND nc_widget_create_group(HWND parent, int id, const char *title, int x, int y, int width,
                            int height);
HWND nc_widget_create_edit(HWND parent, int id, const char *text, int x, int y, int width,
                           int height, bool read_only, NcAlign align, const char *cue);
HWND nc_widget_create_combo(HWND parent, int id, int x, int y, int width, int height);
void nc_widget_set_text(NcWidgets *widgets, HWND hwnd, const char *text);
void nc_widget_set_text_color(NcWidgets *widgets, HWND hwnd, uint32_t color);
void nc_widget_set_button_colors(NcWidgets *widgets, HWND hwnd, uint32_t background,
                                 uint32_t foreground);
void nc_widget_apply_fonts(NcWidgets *widgets, const NcFonts *fonts);
void nc_widget_draw_item(const DRAWITEMSTRUCT *draw, const NcWidgets *widgets,
                         const NcFonts *fonts);

typedef struct {
    HWND edit;
    HWND updown;
    int32_t max;
    int32_t increment;
} NcNumberInput;

void nc_number_create(NcNumberInput *input, HWND parent, int id, int x, int y, int width,
                      int height, int32_t max, int32_t increment);
bool nc_number_parsed(const NcNumberInput *input, int32_t *out);
bool nc_number_valid(const NcNumberInput *input);
int32_t nc_number_value(const NcNumberInput *input);
void nc_number_set(const NcNumberInput *input, int32_t value);
void nc_number_clamp(NcNumberInput *input);
bool nc_number_delta(NcNumberInput *input, int32_t delta);
bool nc_parse_number_input(const char *text, int32_t max, int32_t *out);
void nc_format_number(int32_t value, char *buffer, size_t cap);

void nc_list_create(HWND *out, HWND parent, int id, int x, int y, int width, int height);
void nc_list_add_column(HWND list, const char *text, int width);
void nc_list_clear(HWND list);
void nc_list_insert_row(HWND list, int index, const char *text, bool checked);
void nc_list_set_checked(HWND list, int index, bool checked);
bool nc_list_is_checked(HWND list, int index);
void nc_list_set_font(HWND list, HFONT font);
