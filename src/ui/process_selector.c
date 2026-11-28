#include "ui/process_selector.h"

#include <commctrl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "core/win.h"
#include "engine/process_filter.h"
#include "ui/ids.h"

typedef struct {
    char *name;
    uint32_t count;
    char *path;
} SelectorRow;

typedef struct {
    NcUiHost host;
    HWND hwnd;
    HWND search_edit;
    HWND list;
    HWND empty_label;
    HWND count_label;
    SelectorRow *rows;
    size_t row_count;
    size_t row_cap;
    size_t *displayed;
    size_t displayed_count;
    size_t displayed_cap;
    NcStringList selected;
    bool suppress;
    NcWidgets widgets;
    NcFonts fonts;
    bool has_outcome;
    NcStringList outcome;
} ProcessSelector;

static int scale_of(int value, double scale) {
    return (int)((double)value * scale + 0.5);
}

static void rows_free(ProcessSelector *selector) {
    size_t index;
    for (index = 0; index < selector->row_count; index++) {
        free(selector->rows[index].name);
        free(selector->rows[index].path);
    }
    selector->row_count = 0;
}

static void row_push(ProcessSelector *selector, const char *name, uint32_t count,
                     const char *path) {
    SelectorRow *row;
    if (selector->row_count == selector->row_cap) {
        size_t cap = selector->row_cap ? selector->row_cap * 2 : 64;
        SelectorRow *rows = (SelectorRow *)realloc(selector->rows, cap * sizeof(SelectorRow));
        if (!rows) {
            return;
        }
        selector->rows = rows;
        selector->row_cap = cap;
    }
    row = &selector->rows[selector->row_count++];
    row->name = nc_strdup(name ? name : "");
    row->count = count;
    row->path = nc_strdup(path ? path : "");
}

static void displayed_push(ProcessSelector *selector, size_t index) {
    if (selector->displayed_count == selector->displayed_cap) {
        size_t cap = selector->displayed_cap ? selector->displayed_cap * 2 : 64;
        size_t *items = (size_t *)realloc(selector->displayed, cap * sizeof(size_t));
        if (!items) {
            return;
        }
        selector->displayed = items;
        selector->displayed_cap = cap;
    }
    selector->displayed[selector->displayed_count++] = index;
}

static bool selected_contains(ProcessSelector *selector, const char *name) {
    return nc_string_list_contains_ci(&selector->selected, name);
}

static void selected_add(ProcessSelector *selector, const char *name) {
    if (!selected_contains(selector, name)) {
        nc_string_list_push(&selector->selected, name);
    }
}

static void selected_remove(ProcessSelector *selector, const char *name) {
    size_t index = 0;
    while (index < selector->selected.count) {
        if (nc_ieq_ascii(selector->selected.items[index], name)) {
            nc_string_list_remove_at(&selector->selected, index);
        } else {
            index++;
        }
    }
}

static void lower_copy(const char *text, Str *out) {
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    str_clear(out);
    while (*cursor) {
        char c = (char)tolower(*cursor);
        str_addn(out, &c, 1);
        cursor++;
    }
}

static int lower_compare(const char *left, const char *right) {
    while (*left && *right) {
        int a = tolower((unsigned char)*left);
        int b = tolower((unsigned char)*right);
        if (a != b) {
            return a < b ? -1 : 1;
        }
        left++;
        right++;
    }
    if (*left == *right) {
        return 0;
    }
    return *left ? 1 : -1;
}

static void update_count(ProcessSelector *selector) {
    Str text;
    str_init(&text);
    if (selector->selected.count == 0) {
        str_add(&text, "当前：全局");
    } else {
        str_addf(&text, "已选 %u 个", (unsigned)selector->selected.count);
    }
    nc_widget_set_text(&selector->widgets, selector->count_label, str_cstr(&text));
    str_free(&text);
}

static void rebuild(ProcessSelector *selector) {
    char *raw_filter;
    char *filter;
    Str lower_filter;
    size_t *order;
    size_t order_count;
    size_t index;
    size_t i;
    LRESULT list_index;
    if (!selector) {
        return;
    }
    raw_filter = nc_get_text(selector->search_edit);
    filter = raw_filter ? nc_trim(raw_filter) : NULL;
    str_init(&lower_filter);
    lower_copy(filter ? filter : "", &lower_filter);
    selector->suppress = true;
    nc_list_clear(selector->list);
    selector->displayed_count = 0;
    order_count = selector->row_count;
    order = order_count ? (size_t *)malloc(order_count * sizeof(size_t)) : NULL;
    if (order_count > 0 && !order) {
        str_free(&lower_filter);
        free(raw_filter);
        selector->suppress = false;
        return;
    }
    for (index = 0; index < order_count; index++) {
        order[index] = index;
    }
    for (i = 1; i < order_count; i++) {
        size_t key = order[i];
        size_t j = i;
        while (j > 0) {
            size_t other = order[j - 1];
            bool left_selected = selected_contains(selector, selector->rows[other].name);
            bool right_selected = selected_contains(selector, selector->rows[key].name);
            int left_rank = left_selected ? 0 : (selector->rows[other].count == 0 ? 2 : 1);
            int right_rank = right_selected ? 0 : (selector->rows[key].count == 0 ? 2 : 1);
            int comparison = left_rank - right_rank;
            if (comparison == 0) {
                comparison = lower_compare(selector->rows[other].name, selector->rows[key].name);
            }
            if (comparison <= 0) {
                break;
            }
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    list_index = 0;
    for (i = 0; i < order_count; i++) {
        size_t row_index = order[i];
        SelectorRow *row = &selector->rows[row_index];
        bool matched = true;
        if (lower_filter.len > 0) {
            Str lower_name;
            str_init(&lower_name);
            lower_copy(row->name, &lower_name);
            matched = strstr(str_cstr(&lower_name), str_cstr(&lower_filter)) != NULL;
            str_free(&lower_name);
        }
        if (matched) {
            bool checked = selected_contains(selector, row->name);
            nc_list_insert_row(selector->list, (int)list_index, row->name, checked);
            displayed_push(selector, row_index);
            list_index++;
        }
    }
    free(order);
    selector->suppress = false;
    nc_show(selector->empty_label, selector->displayed_count == 0 ? SW_SHOW : SW_HIDE);
    update_count(selector);
    str_free(&lower_filter);
    free(raw_filter);
}

static void reload(ProcessSelector *selector) {
    NcProcessList processes;
    size_t index;
    size_t selected_index;
    if (!selector) {
        return;
    }
    nc_process_enumerate(&processes);
    rows_free(selector);
    for (index = 0; index < processes.count; index++) {
        const char *name = processes.items[index].name;
        size_t found;
        for (found = 0; found < selector->row_count; found++) {
            if (nc_ieq_ascii(selector->rows[found].name, name)) {
                break;
            }
        }
        if (found < selector->row_count) {
            SelectorRow *row = &selector->rows[found];
            row->count++;
            if ((!row->path || !row->path[0]) && processes.items[index].path) {
                free(row->path);
                row->path = nc_strdup(processes.items[index].path);
            }
        } else {
            row_push(selector, name, 1, processes.items[index].path);
        }
    }
    nc_process_list_free(&processes);
    for (selected_index = 0; selected_index < selector->selected.count; selected_index++) {
        const char *name = selector->selected.items[selected_index];
        size_t found;
        for (found = 0; found < selector->row_count; found++) {
            if (nc_ieq_ascii(selector->rows[found].name, name)) {
                break;
            }
        }
        if (found == selector->row_count) {
            row_push(selector, name, 0, "");
        }
    }
    rebuild(selector);
}

static void select_all_visible(ProcessSelector *selector) {
    size_t index;
    selector->suppress = true;
    for (index = 0; index < selector->displayed_count; index++) {
        size_t row_index = selector->displayed[index];
        selected_add(selector, selector->rows[row_index].name);
        nc_list_set_checked(selector->list, (int)index, true);
    }
    selector->suppress = false;
    update_count(selector);
}

static void clear_selection(ProcessSelector *selector) {
    nc_string_list_clear(&selector->selected);
    rebuild(selector);
}

static int compare_selected(const void *left, const void *right) {
    const char *a = *(const char *const *)left;
    const char *b = *(const char *const *)right;
    return lower_compare(a, b);
}

static void confirm_selector(ProcessSelector *selector) {
    size_t index;
    if (selector->selected.count > NC_MAX_PROCESS_COUNT) {
        nc_ui_host_message(&selector->host, "最多选择 64 个进程。", "选择目标进程",
                           MB_OK | MB_ICONWARNING);
        return;
    }
    nc_string_list_clear(&selector->outcome);
    for (index = 0; index < selector->selected.count; index++) {
        nc_string_list_push(&selector->outcome, selector->selected.items[index]);
    }
    if (selector->outcome.count > 1) {
        qsort(selector->outcome.items, selector->outcome.count, sizeof(char *), compare_selected);
    }
    selector->has_outcome = true;
    nc_destroy(selector->hwnd);
}

static LRESULT CALLBACK selector_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    ProcessSelector *selector;
    LRESULT result = 0;
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    selector = (ProcessSelector *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!selector) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (message != WM_CTLCOLORSTATIC &&
        nc_ui_handle_chrome(&selector->widgets, &selector->fonts, selector->host.white_brush, hwnd,
                            message, wparam, lparam, NC_COLOR_SURFACE, &result)) {
        return result;
    }
    switch (message) {
    case WM_COMMAND: {
        int id = (int)LOWORD(wparam);
        UINT code = HIWORD(wparam);
        if (id == NC_ID_SELECTOR_SEARCH && code == EN_CHANGE) {
            rebuild(selector);
        } else if (id == NC_ID_SELECTOR_REFRESH) {
            reload(selector);
        } else if (id == NC_ID_SELECTOR_SELECT_ALL) {
            select_all_visible(selector);
        } else if (id == NC_ID_SELECTOR_CLEAR) {
            clear_selection(selector);
        } else if (id == NC_ID_SELECTOR_OK) {
            confirm_selector(selector);
        } else if (id == NC_ID_SELECTOR_CANCEL) {
            nc_destroy(hwnd);
        }
        return 0;
    }
    case WM_NOTIFY:
        if (lparam) {
            NMHDR *header = (NMHDR *)lparam;
            if (header->code == LVN_ITEMCHANGED && !selector->suppress) {
                NMLISTVIEW *item = (NMLISTVIEW *)lparam;
                int index = item->iItem;
                if (index >= 0 && (((UINT)item->uNewState ^ (UINT)item->uOldState) & 0xF000u) != 0) {
                    if ((size_t)index < selector->displayed_count) {
                        size_t row_index = selector->displayed[index];
                        const char *name = selector->rows[row_index].name;
                        if (nc_list_is_checked(selector->list, index)) {
                            selected_add(selector, name);
                        } else {
                            selected_remove(selector, name);
                        }
                        update_count(selector);
                    }
                }
                return 0;
            }
            if (header->code == NM_DBLCLK) {
                NMITEMACTIVATE *item = (NMITEMACTIVATE *)lparam;
                int index = item->iItem;
                if (index >= 0) {
                    nc_list_set_checked(selector->list, index,
                                        !nc_list_is_checked(selector->list, index));
                }
                return 0;
            }
            if (header->code == LVN_GETINFOTIP) {
                NMLVGETINFOTIPW *info = (NMLVGETINFOTIPW *)lparam;
                int index = info->iItem;
                if (index >= 0 && (size_t)index < selector->displayed_count) {
                    SelectorRow *row = &selector->rows[selector->displayed[index]];
                    Str text;
                    str_init(&text);
                    if (row->count == 0) {
                        str_add(&text, "当前未运行（按进程名匹配，启动后生效）");
                    } else if (!row->path || !row->path[0]) {
                        str_addf(&text, "实例：%u", (unsigned)row->count);
                    } else {
                        str_addf(&text, "实例：%u\n%s", (unsigned)row->count, row->path);
                    }
                    if (info->pszText && info->cchTextMax > 0) {
                        nc_write_fixed(info->pszText, (size_t)info->cchTextMax, str_cstr(&text));
                    }
                    str_free(&text);
                }
                return 0;
            }
        }
        return 0;
    case WM_CLOSE:
        nc_destroy(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

static BOOL CALLBACK register_selector_class([[maybe_unused]] PINIT_ONCE once,
                                             [[maybe_unused]] PVOID param,
                                             [[maybe_unused]] PVOID *context) {
    return nc_register_class(L"NCProcessSelector", selector_wnd_proc) ? TRUE : FALSE;
}

bool nc_process_selector_run(const NcUiHost *host, const NcStringList *current_selected,
                             NcStringList *out) {
    static INIT_ONCE class_once = INIT_ONCE_STATIC_INIT;
    ProcessSelector *selector;
    double preferred;
    double scale;
    HWND parent;
    bool ok = false;
    if (!host || !out) {
        return false;
    }
    nc_string_list_init(out);
    if (!InitOnceExecuteOnce(&class_once, register_selector_class, NULL, NULL)) {
        return false;
    }
    preferred = host->scale > 0 ? host->scale : 1.0;
    scale = nc_fit_dialog_scale(host->owner, 460, 520, preferred);
    selector = (ProcessSelector *)calloc(1, sizeof(ProcessSelector));
    if (!selector) {
        return false;
    }
    selector->host = *host;
    nc_string_list_init(&selector->selected);
    nc_string_list_init(&selector->outcome);
    nc_widgets_init(&selector->widgets);
    nc_fonts_init(&selector->fonts, (UINT)(scale * 96.0 + 0.5));
    selector->hwnd = nc_create_window(L"NCProcessSelector", L"选择目标进程", NC_STYLE_DIALOG,
                                      WS_EX_CONTROLPARENT, host->owner, 0, selector);
    if (!selector->hwnd) {
        goto cleanup;
    }
    nc_set_window_client_size(selector->hwnd, scale_of(460, scale), scale_of(520, scale));
    nc_center_on_owner(selector->hwnd, host->owner);
    parent = selector->hwnd;
#define S(v) scale_of((v), scale)
    nc_widget_create_text(&selector->widgets, parent, NC_ID_SELECTOR_TITLE, "选择目标进程",
                          S(16), S(14), S(300), S(24), NC_FONT_HOTKEY, NC_COLOR_TEXT_DARK,
                          NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
    nc_widget_create_text(&selector->widgets, parent, NC_ID_SELECTOR_SUBTITLE,
                          "勾选后仅对这些程序生效；不勾选 = 全局（所有进程）", S(16), S(42), S(420),
                          S(20), NC_FONT_SMALL85, NC_COLOR_TEXT_MUTED, NC_COLOR_SURFACE,
                          NC_ALIGN_LEFT, false, false);
    selector->search_edit = nc_widget_create_edit(parent, NC_ID_SELECTOR_SEARCH, "", S(16), S(68),
                                                  S(372), S(28), false, NC_ALIGN_LEFT,
                                                  "搜索进程名…");
    nc_set_font(selector->search_edit, nc_fonts_get(&selector->fonts, NC_FONT_BODY));
    nc_widget_create_button(&selector->widgets, parent, NC_ID_SELECTOR_REFRESH, "刷新", S(396),
                            S(68), S(48), S(28), NC_COLOR_CARD, NC_COLOR_TEXT_BUTTON,
                            NC_FONT_BODY, NC_COLOR_GRAY, true, NC_COLOR_BORDER, false);
    nc_list_create(&selector->list, parent, NC_ID_SELECTOR_LIST, S(16), S(108), S(428), S(332));
    nc_list_add_column(selector->list, "进程", S(384));
    nc_list_set_font(selector->list, nc_fonts_get(&selector->fonts, NC_FONT_BODY));
    selector->empty_label = nc_widget_create_text(
        &selector->widgets, parent, NC_ID_SELECTOR_EMPTY, "没有匹配的进程", S(17), S(129),
        S(426), S(309), NC_FONT_BODY, NC_COLOR_TEXT_MUTED, NC_COLOR_CARD, NC_ALIGN_CENTER,
        false, true);
    nc_show(selector->empty_label, SW_HIDE);
    selector->count_label = nc_widget_create_text(&selector->widgets, parent, NC_ID_SELECTOR_COUNT,
                                                  "当前：全局", S(16), S(452), S(200), S(20),
                                                  NC_FONT_SMALL85, NC_COLOR_TEXT_MUTED,
                                                  NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
    nc_widget_create_button(&selector->widgets, parent, NC_ID_SELECTOR_SELECT_ALL, "全选", S(326),
                            S(449), S(40), S(24), NC_COLOR_SURFACE, NC_COLOR_ACCENT,
                            NC_FONT_LINK85, NC_COLOR_SURFACE, false, 0, true);
    nc_widget_create_button(&selector->widgets, parent, NC_ID_SELECTOR_CLEAR, "清除", S(376),
                            S(449), S(40), S(24), NC_COLOR_SURFACE, NC_COLOR_ACCENT,
                            NC_FONT_LINK85, NC_COLOR_SURFACE, false, 0, true);
    nc_widget_create_button(&selector->widgets, parent, NC_ID_SELECTOR_CANCEL, "取消", S(252),
                            S(478), S(92), S(32), NC_COLOR_CARD, NC_COLOR_TEXT_BUTTON,
                            NC_FONT_BODY, NC_COLOR_GRAY, true, NC_COLOR_BORDER, false);
    nc_widget_create_button(&selector->widgets, parent, NC_ID_SELECTOR_OK, "确定", S(352), S(478),
                            S(92), S(32), NC_COLOR_ACCENT, 0x00FFFFFF, NC_FONT_BODY_BOLD,
                            NC_COLOR_GRAY, false, 0, false);
#undef S
    nc_widget_apply_fonts(&selector->widgets, &selector->fonts);
    if (current_selected) {
        size_t index;
        for (index = 0; index < current_selected->count; index++) {
            Str display;
            str_init(&display);
            nc_config_normalize_display_name(current_selected->items[index], &display);
            if (display.len > 0) {
                selected_add(selector, str_cstr(&display));
            }
            str_free(&display);
        }
    }
    reload(selector);
    nc_run_modal(host->owner, selector->hwnd);
    if (selector->has_outcome) {
        *out = selector->outcome;
        nc_string_list_init(&selector->outcome);
        ok = true;
    }
cleanup:
    rows_free(selector);
    free(selector->rows);
    free(selector->displayed);
    nc_string_list_free(&selector->selected);
    nc_string_list_free(&selector->outcome);
    nc_widgets_free(&selector->widgets);
    nc_fonts_destroy(&selector->fonts);
    free(selector);
    return ok;
}
