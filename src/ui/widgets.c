#include "ui/widgets.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/str.h"

static const struct {
    const wchar_t *family;
    UINT size_x10;
    bool bold;
    bool underline;
} FONT_TABLE[NC_FONT_ROLE_COUNT] = {
    {L"Microsoft YaHei UI", 170, true, false},
    {L"Microsoft YaHei UI", 85, false, false},
    {L"Microsoft YaHei UI", 160, true, false},
    {L"Segoe UI", 280, false, false},
    {L"Microsoft YaHei UI", 90, false, false},
    {L"Microsoft YaHei UI", 90, true, false},
    {L"Microsoft YaHei UI", 80, false, false},
    {L"Microsoft YaHei UI", 85, false, false},
    {L"Microsoft YaHei UI", 140, true, false},
    {L"Microsoft YaHei UI", 125, true, false},
    {L"Microsoft YaHei UI", 110, true, false},
    {L"Microsoft YaHei UI", 90, false, true},
    {L"Microsoft YaHei UI", 85, false, true},
};

void nc_fonts_init(NcFonts *fonts, UINT dpi) {
    int index;
    if (!fonts) {
        return;
    }
    for (index = 0; index < NC_FONT_ROLE_COUNT; index++) {
        fonts->fonts[index] = nc_create_font(FONT_TABLE[index].family, FONT_TABLE[index].size_x10,
                                             dpi, FONT_TABLE[index].bold, FONT_TABLE[index].underline);
    }
}

void nc_fonts_destroy(NcFonts *fonts) {
    int index;
    if (!fonts) {
        return;
    }
    for (index = 0; index < NC_FONT_ROLE_COUNT; index++) {
        nc_delete_object(fonts->fonts[index]);
        fonts->fonts[index] = NULL;
    }
}

HFONT nc_fonts_get(const NcFonts *fonts, NcFontRole role) {
    if (!fonts || role < 0 || role >= NC_FONT_ROLE_COUNT) {
        return NULL;
    }
    return fonts->fonts[role];
}

void nc_widgets_init(NcWidgets *widgets) {
    if (widgets) {
        widgets->items = NULL;
        widgets->count = 0;
        widgets->cap = 0;
    }
}

void nc_widgets_clear(NcWidgets *widgets) {
    size_t index;
    if (!widgets) {
        return;
    }
    for (index = 0; index < widgets->count; index++) {
        free(widgets->items[index].text);
    }
    widgets->count = 0;
}

void nc_widgets_free(NcWidgets *widgets) {
    if (!widgets) {
        return;
    }
    nc_widgets_clear(widgets);
    free(widgets->items);
    widgets->items = NULL;
    widgets->cap = 0;
}

NcWidget *nc_widget_find(NcWidgets *widgets, HWND hwnd) {
    size_t index;
    if (!widgets || !hwnd) {
        return NULL;
    }
    for (index = 0; index < widgets->count; index++) {
        if (widgets->items[index].hwnd == hwnd) {
            return &widgets->items[index];
        }
    }
    return NULL;
}

static NcWidget *widget_add(NcWidgets *widgets, HWND hwnd, NcWidgetKind kind) {
    NcWidget widget;
    if (!widgets || !hwnd) {
        return NULL;
    }
    if (widgets->count == widgets->cap) {
        size_t cap = widgets->cap ? widgets->cap * 2 : 32;
        NcWidget *items = (NcWidget *)realloc(widgets->items, cap * sizeof(NcWidget));
        if (!items) {
            return NULL;
        }
        widgets->items = items;
        widgets->cap = cap;
    }
    memset(&widget, 0, sizeof(widget));
    widget.hwnd = hwnd;
    widget.kind = kind;
    widgets->items[widgets->count++] = widget;
    return &widgets->items[widgets->count - 1];
}

void nc_widgets_remove_window(NcWidgets *widgets, HWND window) {
    size_t index = 0;
    if (!widgets || !window) {
        return;
    }
    while (index < widgets->count) {
        HWND root = GetAncestor(widgets->items[index].hwnd, GA_ROOT);
        if (widgets->items[index].hwnd == window || root == window) {
            free(widgets->items[index].text);
            memmove(&widgets->items[index], &widgets->items[index + 1],
                    (widgets->count - index - 1) * sizeof(NcWidget));
            widgets->count--;
        } else {
            index++;
        }
    }
}

HWND nc_widget_create_panel(NcWidgets *widgets, HWND parent, int id, int x, int y, int width,
                            int height, uint32_t background, bool has_border, uint32_t border) {
    HWND hwnd = nc_create_child(parent, L"STATIC", L"", SS_OWNERDRAW | WS_CHILD | WS_VISIBLE, 0, x,
                                y, width, height, id);
    NcWidget *widget = widget_add(widgets, hwnd, NC_WIDGET_PANEL);
    if (widget) {
        widget->background = background;
        widget->has_border = has_border;
        widget->border = border;
    }
    return hwnd;
}

HWND nc_widget_create_text(NcWidgets *widgets, HWND parent, int id, const char *text, int x, int y,
                           int width, int height, NcFontRole font, uint32_t color,
                           uint32_t background, NcAlign align, bool multiline, bool vcenter) {
    wchar_t *wide = nc_wide_dup(text ? text : "");
    HWND hwnd;
    NcWidget *widget;
    if (!wide) {
        return NULL;
    }
    hwnd = nc_create_child(parent, L"STATIC", wide, SS_OWNERDRAW | WS_CHILD | WS_VISIBLE, 0, x, y,
                           width, height, id);
    free(wide);
    widget = widget_add(widgets, hwnd, NC_WIDGET_TEXT);
    if (widget) {
        widget->text = nc_strdup(text ? text : "");
        widget->font = font;
        widget->color = color;
        widget->background = background;
        widget->align = align;
        widget->multiline = multiline;
        widget->vcenter = vcenter;
    }
    return hwnd;
}

HWND nc_widget_create_button(NcWidgets *widgets, HWND parent, int id, const char *text, int x,
                             int y, int width, int height, uint32_t background, uint32_t foreground,
                             NcFontRole font, uint32_t disabled_background, bool has_border,
                             uint32_t border, bool link) {
    wchar_t *wide = nc_wide_dup(text ? text : "");
    DWORD style = BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE;
    HWND hwnd;
    NcWidget *widget;
    if (!wide) {
        return NULL;
    }
    if (!link) {
        style |= WS_TABSTOP;
    }
    hwnd = nc_create_child(parent, L"BUTTON", wide, style, 0, x, y, width, height, id);
    free(wide);
    widget = widget_add(widgets, hwnd, NC_WIDGET_BUTTON);
    if (widget) {
        widget->text = nc_strdup(text ? text : "");
        widget->button_bg = background;
        widget->button_fg = foreground;
        widget->font = font;
        widget->disabled_bg = disabled_background;
        widget->has_border = has_border;
        widget->border = border;
    }
    return hwnd;
}

HWND nc_widget_create_group(HWND parent, int id, const char *title, int x, int y, int width,
                            int height) {
    wchar_t *wide = nc_wide_dup(title ? title : "");
    HWND hwnd;
    if (!wide) {
        return NULL;
    }
    hwnd = nc_create_child(parent, L"BUTTON", wide,
                           BS_GROUPBOX | WS_CHILD | WS_VISIBLE, 0, x, y, width, height, id);
    free(wide);
    return hwnd;
}

HWND nc_widget_create_edit(HWND parent, int id, const char *text, int x, int y, int width,
                           int height, bool read_only, NcAlign align, const char *cue) {
    wchar_t *wide = nc_wide_dup(text ? text : "");
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
    HWND hwnd;
    if (!wide) {
        return NULL;
    }
    if (read_only) {
        style |= ES_READONLY;
    }
    if (align == NC_ALIGN_CENTER) {
        style |= ES_CENTER;
    }
    hwnd = nc_create_child(parent, L"EDIT", wide, style, WS_EX_CLIENTEDGE, x, y, width, height, id);
    free(wide);
    if (cue) {
        nc_set_cue_banner(hwnd, cue);
    }
    return hwnd;
}

HWND nc_widget_create_combo(HWND parent, int id, int x, int y, int width, int height) {
    return nc_create_child(parent, L"COMBOBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                           x, y, width, height, id);
}

void nc_widget_set_text(NcWidgets *widgets, HWND hwnd, const char *text) {
    NcWidget *widget = nc_widget_find(widgets, hwnd);
    if (widget && widget->kind == NC_WIDGET_TEXT) {
        char *copy = nc_strdup(text ? text : "");
        if (copy) {
            free(widget->text);
            widget->text = copy;
        }
    }
    nc_set_text(hwnd, text);
    nc_invalidate(hwnd);
}

void nc_widget_set_text_color(NcWidgets *widgets, HWND hwnd, uint32_t color) {
    NcWidget *widget = nc_widget_find(widgets, hwnd);
    if (widget && widget->kind == NC_WIDGET_TEXT) {
        widget->color = color;
    }
    nc_invalidate(hwnd);
}

void nc_widget_set_button_colors(NcWidgets *widgets, HWND hwnd, uint32_t background,
                                 uint32_t foreground) {
    NcWidget *widget = nc_widget_find(widgets, hwnd);
    if (widget && widget->kind == NC_WIDGET_BUTTON) {
        widget->button_bg = background;
        widget->button_fg = foreground;
    }
    nc_invalidate(hwnd);
}

void nc_widget_apply_fonts(NcWidgets *widgets, const NcFonts *fonts) {
    size_t index;
    if (!widgets || !fonts) {
        return;
    }
    for (index = 0; index < widgets->count; index++) {
        NcWidget *widget = &widgets->items[index];
        if (widget->kind == NC_WIDGET_PANEL) {
            continue;
        }
        nc_set_font(widget->hwnd, nc_fonts_get(fonts, widget->font));
    }
}

void nc_widget_draw_item(const DRAWITEMSTRUCT *draw, const NcWidgets *widgets,
                         const NcFonts *fonts) {
    const NcWidget *widget;
    if (!draw || !widgets || !fonts) {
        return;
    }
    widget = nc_widget_find((NcWidgets *)widgets, draw->hwndItem);
    if (!widget) {
        return;
    }
    if (widget->kind == NC_WIDGET_PANEL) {
        nc_fill_rect(draw->hDC, &draw->rcItem, widget->background);
        if (widget->has_border) {
            nc_frame_rect(draw->hDC, &draw->rcItem, widget->border);
        }
        return;
    }
    if (widget->kind == NC_WIDGET_TEXT) {
        UINT format = DT_NOPREFIX;
        RECT rect = draw->rcItem;
        nc_fill_rect(draw->hDC, &draw->rcItem, widget->background);
        if (widget->align == NC_ALIGN_CENTER) {
            format |= DT_CENTER;
        } else {
            format |= DT_LEFT;
        }
        if (widget->multiline) {
            format |= DT_WORDBREAK;
        } else {
            format |= DT_SINGLELINE;
            if (widget->vcenter) {
                format |= DT_VCENTER;
            }
        }
        if (!widget->multiline) {
            rect.left += 2;
            rect.right -= 2;
        }
        nc_draw_text(draw->hDC, widget->text ? widget->text : "", &rect,
                     nc_fonts_get(fonts, widget->font), widget->color, format);
        return;
    }
    {
        bool disabled = (draw->itemState & ODS_DISABLED) != 0;
        uint32_t background = disabled ? widget->disabled_bg : widget->button_bg;
        char *text;
        UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        RECT rect = draw->rcItem;
        nc_fill_rect(draw->hDC, &draw->rcItem, background);
        if (widget->has_border) {
            nc_frame_rect(draw->hDC, &draw->rcItem, widget->border);
        }
        if ((draw->itemState & ODS_FOCUS) != 0) {
            RECT focus = draw->rcItem;
            focus.left += 3;
            focus.top += 3;
            focus.right -= 3;
            focus.bottom -= 3;
            nc_frame_rect(draw->hDC, &focus, RGB(33, 150, 243));
        }
        text = nc_get_text(draw->hwndItem);
        nc_draw_text(draw->hDC, text ? text : "", &rect, nc_fonts_get(fonts, widget->font),
                     widget->button_fg, format);
        free(text);
    }
}

bool nc_parse_number_input(const char *text, int32_t max, int32_t *out) {
    return nc_parse_i32_digits(text, max, out);
}

void nc_format_number(int32_t value, char *buffer, size_t cap) {
    nc_format_i32_grouped(value, buffer, cap);
}

void nc_number_create(NcNumberInput *input, HWND parent, int id, int x, int y, int width,
                      int height, int32_t max, int32_t increment) {
    int updown_width;
    int edit_width;
    if (!input) {
        return;
    }
    updown_width = (int)((double)height * 0.75 + 0.5);
    if (updown_width < 15) {
        updown_width = 15;
    }
    edit_width = width - updown_width;
    if (edit_width < 40) {
        edit_width = 40;
    }
    input->edit = nc_create_child(parent, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT,
                                  WS_EX_CLIENTEDGE, x, y, edit_width, height, id);
    input->updown = nc_create_child(parent, L"msctls_updown32", L"", WS_CHILD | WS_VISIBLE, 0,
                                    x + edit_width, y, updown_width, height, id + 5000);
    input->max = max;
    input->increment = increment;
    SendMessageW(input->updown, UDM_SETRANGE32, 0, (LPARAM)max);
}

bool nc_number_parsed(const NcNumberInput *input, int32_t *out) {
    char *text;
    bool ok;
    if (!input || !input->edit) {
        return false;
    }
    text = nc_get_text(input->edit);
    if (!text) {
        return false;
    }
    ok = nc_parse_number_input(text, input->max, out);
    free(text);
    return ok;
}

bool nc_number_valid(const NcNumberInput *input) {
    int32_t value;
    return nc_number_parsed(input, &value);
}

int32_t nc_number_value(const NcNumberInput *input) {
    int32_t value = 0;
    if (!nc_number_parsed(input, &value)) {
        return 0;
    }
    return value;
}

void nc_number_set(const NcNumberInput *input, int32_t value) {
    char buffer[32];
    if (!input) {
        return;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > input->max) {
        value = input->max;
    }
    SendMessageW(input->updown, UDM_SETPOS32, 0, (LPARAM)value);
    nc_format_number(value, buffer, sizeof(buffer));
    nc_set_text(input->edit, buffer);
}

void nc_number_clamp(NcNumberInput *input) {
    int32_t value;
    if (nc_number_parsed(input, &value)) {
        nc_number_set(input, value);
    }
}

bool nc_number_delta(NcNumberInput *input, int32_t delta) {
    int32_t target;
    int64_t result;
    if (!input) {
        return false;
    }
    result = (int64_t)nc_number_value(input) + (int64_t)input->increment * (int64_t)delta;
    if (result > INT32_MAX) {
        target = INT32_MAX;
    } else if (result < INT32_MIN) {
        target = INT32_MIN;
    } else {
        target = (int32_t)result;
    }
    nc_number_set(input, target);
    return true;
}

void nc_list_create(HWND *out, HWND parent, int id, int x, int y, int width, int height) {
    HWND list = nc_create_child(parent, L"SysListView32", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                WS_EX_CLIENTEDGE, x, y, width, height, id);
    SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_DOUBLEBUFFER);
    if (out) {
        *out = list;
    }
}

void nc_list_add_column(HWND list, const char *text, int width) {
    wchar_t *wide = nc_wide_dup(text ? text : "");
    LVCOLUMNW column;
    if (!wide) {
        return;
    }
    memset(&column, 0, sizeof(column));
    column.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    column.fmt = LVCFMT_LEFT;
    column.cx = width;
    column.pszText = wide;
    column.cchTextMax = (int)wcslen(wide) + 1;
    SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column);
    free(wide);
}

void nc_list_clear(HWND list) {
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
}

void nc_list_insert_row(HWND list, int index, const char *text, bool checked) {
    wchar_t *wide = nc_wide_dup(text ? text : "");
    LVITEMW item;
    if (!wide) {
        return;
    }
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = index;
    item.pszText = wide;
    item.cchTextMax = (int)wcslen(wide) + 1;
    item.lParam = index;
    SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    free(wide);
    nc_list_set_checked(list, index, checked);
}

void nc_list_set_checked(HWND list, int index, bool checked) {
    LVITEMW item;
    memset(&item, 0, sizeof(item));
    item.stateMask = LVIS_STATEIMAGEMASK;
    item.state = checked ? (2u << 12) : (1u << 12);
    SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)index, (LPARAM)&item);
}

bool nc_list_is_checked(HWND list, int index) {
    LRESULT state = SendMessageW(list, LVM_GETITEMSTATE, (WPARAM)index, LVIS_STATEIMAGEMASK);
    return ((UINT)state & LVIS_STATEIMAGEMASK) == (2u << 12);
}

void nc_list_set_font(HWND list, HFONT font) {
    nc_set_font(list, font);
}
