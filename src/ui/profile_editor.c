#include "ui/profile_editor.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/str.h"
#include "core/time.h"
#include "core/win.h"
#include "ui/ids.h"

#define NC_EM_SETLIMITTEXT 0x00C5

typedef struct {
    NcUiHost host;
    HWND hwnd;
    char *existing_id;
    NcStringList existing_names;
    HWND name_edit;
    NcNumberInput up[6];
    NcNumberInput down[6];
    HWND protocol_combo;
    NcWidgets widgets;
    NcFonts fonts;
    NcEditorOutcome outcome;
} ProfileEditor;

static int scale_of(int value, double scale) {
    return (int)((double)value * scale + 0.5);
}

static void make_default_name(const NcStringList *existing, Str *out) {
    int index;
    str_clear(out);
    if (!nc_string_list_contains_ci(existing, "新配置")) {
        str_add(out, "新配置");
        return;
    }
    for (index = 2; index < 1000; index++) {
        Str candidate;
        str_init(&candidate);
        str_addf(&candidate, "新配置 %d", index);
        if (!nc_string_list_contains_ci(existing, str_cstr(&candidate))) {
            str_add(out, str_cstr(&candidate));
            str_free(&candidate);
            return;
        }
        str_free(&candidate);
    }
    {
        char id[33];
        nc_new_id(id);
        id[6] = 0;
        str_addf(out, "新配置 %s", id);
    }
}

static void load_values(ProfileEditor *editor, const NcProfile *existing) {
    if (existing) {
        const NcDirectionProfile *directions[2];
        int direction_index;
        int row;
        nc_set_text(editor->name_edit, existing->name ? existing->name : "");
        directions[0] = &existing->uplink;
        directions[1] = &existing->downlink;
        for (direction_index = 0; direction_index < 2; direction_index++) {
            const NcDirectionProfile *d = directions[direction_index];
            int32_t values[6] = {d->delay_ms, d->jitter_ms, d->bandwidth_kbps,
                                 d->random_loss_percent, d->burst_allow_ms, d->burst_drop_ms};
            for (row = 0; row < 6; row++) {
                nc_number_set(direction_index == 0 ? &editor->up[row] : &editor->down[row], values[row]);
            }
        }
        {
            int index = 2;
            if (existing->protocol == NC_PROTOCOL_TCP) {
                index = 0;
            } else if (existing->protocol == NC_PROTOCOL_UDP) {
                index = 1;
            }
            SendMessageW(editor->protocol_combo, CB_SETCURSEL, (WPARAM)index, 0);
        }
    } else {
        Str name;
        str_init(&name);
        make_default_name(&editor->existing_names, &name);
        nc_set_text(editor->name_edit, str_cstr(&name));
        str_free(&name);
    }
}

static NcNumberInput *number_by_id(ProfileEditor *editor, int id) {
    if (id >= NC_ID_EDITOR_UP_BASE && id < NC_ID_EDITOR_UP_BASE + 6) {
        return &editor->up[id - NC_ID_EDITOR_UP_BASE];
    }
    if (id >= NC_ID_EDITOR_DOWN_BASE && id < NC_ID_EDITOR_DOWN_BASE + 6) {
        return &editor->down[id - NC_ID_EDITOR_DOWN_BASE];
    }
    return NULL;
}

static void read_direction(const NcNumberInput *numbers, NcDirectionProfile *out) {
    memset(out, 0, sizeof(*out));
    out->delay_ms = nc_number_value(&numbers[0]);
    out->jitter_ms = nc_number_value(&numbers[1]);
    out->bandwidth_kbps = nc_number_value(&numbers[2]);
    out->random_loss_percent = nc_number_value(&numbers[3]);
    out->burst_allow_ms = nc_number_value(&numbers[4]);
    out->burst_drop_ms = nc_number_value(&numbers[5]);
}

static void confirm_editor(ProfileEditor *editor) {
    char *raw_name = nc_get_text(editor->name_edit);
    char *name;
    NcProfile profile;
    NcErr error;
    int row;
    if (!raw_name) {
        return;
    }
    name = nc_trim(raw_name);
    if (!name[0]) {
        nc_ui_host_message(&editor->host, "请输入配置名称。", "配置无效", MB_OK | MB_ICONWARNING);
        nc_set_focus(editor->name_edit);
        free(raw_name);
        return;
    }
    if (nc_string_list_contains_ci(&editor->existing_names, name)) {
        nc_ui_host_message(&editor->host, "已存在同名配置，请换一个名称。", "配置无效",
                           MB_OK | MB_ICONWARNING);
        nc_set_focus(editor->name_edit);
        free(raw_name);
        return;
    }
    for (row = 0; row < 6; row++) {
        if (!nc_number_valid(&editor->up[row])) {
            nc_ui_host_message(&editor->host, "请输入有效的非负整数，不要输入单位或小数。", "配置无效",
                               MB_OK | MB_ICONWARNING);
            nc_set_focus(editor->up[row].edit);
            free(raw_name);
            return;
        }
    }
    for (row = 0; row < 6; row++) {
        if (!nc_number_valid(&editor->down[row])) {
            nc_ui_host_message(&editor->host, "请输入有效的非负整数，不要输入单位或小数。", "配置无效",
                               MB_OK | MB_ICONWARNING);
            nc_set_focus(editor->down[row].edit);
            free(raw_name);
            return;
        }
    }
    memset(&profile, 0, sizeof(profile));
    profile.id = (editor->existing_id && editor->existing_id[0]) ? nc_strdup(editor->existing_id)
                                                                 : nc_config_new_id();
    profile.name = nc_strdup(name);
    read_direction(editor->up, &profile.uplink);
    read_direction(editor->down, &profile.downlink);
    {
        LRESULT selection = SendMessageW(editor->protocol_combo, CB_GETCURSEL, 0, 0);
        if (selection == 0) {
            profile.protocol = NC_PROTOCOL_TCP;
        } else if (selection == 1) {
            profile.protocol = NC_PROTOCOL_UDP;
        } else {
            profile.protocol = NC_PROTOCOL_ALL;
        }
    }
    if (!nc_profile_validate(&profile, &error)) {
        nc_ui_host_message(&editor->host, error.msg, "配置无效", MB_OK | MB_ICONWARNING);
        nc_config_free_profile(&profile);
        free(raw_name);
        return;
    }
    if (nc_profile_all_zero(&profile)) {
        int answer = nc_ui_host_message(&editor->host,
                                        "所有参数均为 0，启动后不会产生弱网效果。仍要保存吗？",
                                        "配置为空", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            nc_config_free_profile(&profile);
            free(raw_name);
            return;
        }
    }
    nc_config_free_profile(&editor->outcome.profile);
    editor->outcome.kind = NC_EDITOR_OUTCOME_SAVED;
    editor->outcome.profile = profile;
    free(raw_name);
    nc_destroy(editor->hwnd);
}

static void delete_editor(ProfileEditor *editor) {
    int answer = nc_ui_host_message(&editor->host, "确定删除这个配置吗？", "删除配置",
                                    MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) {
        return;
    }
    nc_config_free_profile(&editor->outcome.profile);
    editor->outcome.kind = NC_EDITOR_OUTCOME_DELETED;
    nc_destroy(editor->hwnd);
}

static LRESULT CALLBACK editor_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    ProfileEditor *editor;
    LRESULT result = 0;
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    editor = (ProfileEditor *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!editor) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (nc_ui_handle_chrome(&editor->widgets, &editor->fonts, editor->host.white_brush, hwnd,
                            message, wparam, lparam, NC_COLOR_CARD, &result)) {
        return result;
    }
    switch (message) {
    case WM_COMMAND: {
        int id = (int)LOWORD(wparam);
        UINT code = HIWORD(wparam);
        if (id == NC_ID_EDITOR_OK) {
            confirm_editor(editor);
        } else if (id == NC_ID_EDITOR_CANCEL) {
            nc_destroy(hwnd);
        } else if (id == NC_ID_EDITOR_DELETE && code == BN_CLICKED) {
            delete_editor(editor);
        } else if (code == EN_KILLFOCUS) {
            NcNumberInput *number = number_by_id(editor, id);
            if (number) {
                nc_number_clamp(number);
            }
        }
        return 0;
    }
    case WM_NOTIFY:
        if (lparam) {
            NMHDR *header = (NMHDR *)lparam;
            if (header->code == UDN_DELTAPOS) {
                NMUPDOWN *updown = (NMUPDOWN *)lparam;
                UINT_PTR id = header->idFrom;
                if (id >= 5000) {
                    NcNumberInput *number = number_by_id(editor, (int)(id - 5000));
                    if (number) {
                        nc_number_delta(number, updown->iDelta);
                    }
                }
                return 1;
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

static BOOL CALLBACK register_editor_class([[maybe_unused]] PINIT_ONCE once,
                                           [[maybe_unused]] PVOID param,
                                           [[maybe_unused]] PVOID *context) {
    return nc_register_class(L"NCProfileEditor", editor_wnd_proc) ? TRUE : FALSE;
}

bool nc_profile_editor_run(const NcUiHost *host, const NcProfile *existing, bool allow_delete,
                           const NcStringList *existing_names, NcEditorOutcome *out) {
    static INIT_ONCE class_once = INIT_ONCE_STATIC_INIT;
    static const char *up_labels[6] = {
        "上行延时 (ms)", "上行延时抖动 (ms)", "上行带宽 (kbps)",
        "上行随机丢包 (%)", "上行连续丢包-放行 (ms)", "上行连续丢包-丢包 (ms)"};
    static const char *down_labels[6] = {
        "下行延时 (ms)", "下行延时抖动 (ms)", "下行带宽 (kbps)",
        "下行随机丢包 (%)", "下行连续丢包-放行 (ms)", "下行连续丢包-丢包 (ms)"};
    static const int32_t limits[6] = {60000, 60000, 1000000, 100, 600000, 600000};
    static const int32_t increments[6] = {10, 10, 10, 1, 10, 10};
    ProfileEditor *editor;
    double preferred;
    double scale;
    HWND up_group;
    HWND down_group;
    HWND parent;
    int row;
    int index;
    bool ok = false;
    if (!host || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!InitOnceExecuteOnce(&class_once, register_editor_class, NULL, NULL)) {
        return false;
    }
    preferred = host->scale > 0 ? host->scale : 1.0;
    scale = nc_fit_dialog_scale(host->owner, 760, 592, preferred);
    editor = (ProfileEditor *)calloc(1, sizeof(ProfileEditor));
    if (!editor) {
        return false;
    }
    editor->host = *host;
    editor->existing_id = nc_strdup(existing && existing->id ? existing->id : "");
    nc_string_list_init(&editor->existing_names);
    for (index = 0; existing_names && (size_t)index < existing_names->count; index++) {
        nc_string_list_push(&editor->existing_names, existing_names->items[index]);
    }
    nc_widgets_init(&editor->widgets);
    nc_fonts_init(&editor->fonts, (UINT)(scale * 96.0 + 0.5));
    editor->hwnd = nc_create_window(L"NCProfileEditor", existing ? L"编辑配置" : L"新建配置",
                                    NC_STYLE_DIALOG, WS_EX_CONTROLPARENT, host->owner, 0, editor);
    if (!editor->hwnd) {
        goto cleanup;
    }
    nc_set_window_client_size(editor->hwnd, scale_of(760, scale), scale_of(592, scale));
    nc_center_on_owner(editor->hwnd, host->owner);
    parent = editor->hwnd;
#define S(v) scale_of((v), scale)
    nc_widget_create_panel(&editor->widgets, parent, NC_ID_EDITOR_HEADER, 0, 0, S(760), S(58),
                           NC_COLOR_ACCENT, false, 0);
    nc_widget_create_text(&editor->widgets, parent, NC_ID_EDITOR_TITLE,
                          existing ? "编辑配置" : "新建配置", S(20), S(14), S(400), S(30),
                          NC_FONT_TITLE, 0x00FFFFFF, NC_COLOR_ACCENT, NC_ALIGN_LEFT, false, false);
    nc_widget_create_text(&editor->widgets, parent, NC_ID_EDITOR_NAME_LABEL, "名称", S(20), S(80),
                          S(44), S(20), NC_FONT_BODY, NC_COLOR_TEXT_DARK, NC_COLOR_SURFACE,
                          NC_ALIGN_LEFT, false, false);
    editor->name_edit = nc_widget_create_edit(parent, NC_ID_EDITOR_NAME_EDIT, "", S(70), S(76),
                                              S(670), S(27), false, NC_ALIGN_LEFT, NULL);
    SendMessageW(editor->name_edit, NC_EM_SETLIMITTEXT, 60, 0);
    up_group = nc_widget_create_group(parent, NC_ID_EDITOR_UP_GROUP, "上行", S(20), S(112), S(350),
                                      S(340));
    down_group = nc_widget_create_group(parent, NC_ID_EDITOR_DOWN_GROUP, "下行", S(390), S(112),
                                        S(350), S(340));
    nc_set_font(up_group, nc_fonts_get(&editor->fonts, NC_FONT_BODY));
    nc_set_font(down_group, nc_fonts_get(&editor->fonts, NC_FONT_BODY));
    for (row = 0; row < 6; row++) {
        int y = 112 + 28 + row * 42;
        nc_widget_create_text(&editor->widgets, parent,
                              NC_ID_EDITOR_ROW_LABEL_BASE + row, up_labels[row], S(20 + 14),
                              S(y + 4), S(150), S(20), NC_FONT_BODY, NC_COLOR_TEXT_BUTTON,
                              NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
        nc_number_create(&editor->up[row], parent, NC_ID_EDITOR_UP_BASE + row, S(20 + 170), S(y),
                         S(165), S(24), limits[row], increments[row]);
        nc_set_font(editor->up[row].edit, nc_fonts_get(&editor->fonts, NC_FONT_BODY));
        nc_widget_create_text(&editor->widgets, parent,
                              NC_ID_EDITOR_ROW_LABEL_BASE + 10 + row, down_labels[row],
                              S(390 + 14), S(y + 4), S(150), S(20), NC_FONT_BODY,
                              NC_COLOR_TEXT_BUTTON, NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
        nc_number_create(&editor->down[row], parent, NC_ID_EDITOR_DOWN_BASE + row, S(390 + 170),
                         S(y), S(165), S(24), limits[row], increments[row]);
        nc_set_font(editor->down[row].edit, nc_fonts_get(&editor->fonts, NC_FONT_BODY));
    }
    nc_widget_create_text(&editor->widgets, parent, NC_ID_EDITOR_PROTOCOL_LABEL, "弱网控制协议",
                          S(20), S(470), S(110), S(20), NC_FONT_BODY, NC_COLOR_TEXT_DARK,
                          NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
    editor->protocol_combo = nc_widget_create_combo(parent, NC_ID_EDITOR_PROTOCOL_COMBO, S(130),
                                                    S(466), S(170), S(200));
    {
        static const char *items[3] = {"TCP (1)", "UDP (2)", "ALL (3)"};
        for (index = 0; index < 3; index++) {
            wchar_t *wide = nc_wide_dup(items[index]);
            if (wide) {
                SendMessageW(editor->protocol_combo, CB_ADDSTRING, 0, (LPARAM)wide);
                free(wide);
            }
        }
    }
    SendMessageW(editor->protocol_combo, CB_SETCURSEL, 2, 0);
    nc_set_font(editor->protocol_combo, nc_fonts_get(&editor->fonts, NC_FONT_BODY));
    nc_widget_create_text(&editor->widgets, parent, NC_ID_EDITOR_HINT,
                          "带宽单位 kbps；随机丢包单位 %；延时 / 连续丢包单位 ms；0 表示不启用。\r\n"
                          "连续丢包：放行 X ms、丢包 Y ms 循环；只填“丢包”表示一直丢包。上行丢包/限速也会影响下载 ACK。",
                          S(20), S(502), S(720), S(38), NC_FONT_SMALL, NC_COLOR_TEXT_MUTED,
                          NC_COLOR_SURFACE, NC_ALIGN_LEFT, true, false);
    nc_widget_create_button(&editor->widgets, parent, NC_ID_EDITOR_CANCEL, "取消", S(520),
                            S(548), S(100), S(30), NC_COLOR_CARD, NC_COLOR_TEXT_BUTTON,
                            NC_FONT_BODY, NC_COLOR_GRAY, true, NC_COLOR_BORDER, false);
    nc_widget_create_button(&editor->widgets, parent, NC_ID_EDITOR_OK, "确定", S(630), S(548),
                            S(110), S(30), NC_COLOR_ACCENT, 0x00FFFFFF, NC_FONT_BODY_BOLD,
                            NC_COLOR_GRAY, false, 0, false);
    if (allow_delete) {
        nc_widget_create_button(&editor->widgets, parent, NC_ID_EDITOR_DELETE, "删除配置", S(20),
                                S(548), S(110), S(30), NC_COLOR_DANGER_BG, NC_COLOR_DANGER_TEXT,
                                NC_FONT_BODY, NC_COLOR_GRAY, true, NC_COLOR_DANGER_BORDER, false);
    }
#undef S
    nc_widget_apply_fonts(&editor->widgets, &editor->fonts);
    load_values(editor, existing);
    nc_run_modal(host->owner, editor->hwnd);
    if (editor->outcome.kind != NC_EDITOR_OUTCOME_NONE) {
        *out = editor->outcome;
        memset(&editor->outcome, 0, sizeof(editor->outcome));
        ok = true;
    }
cleanup:
    nc_config_free_profile(&editor->outcome.profile);
    nc_widgets_free(&editor->widgets);
    nc_fonts_destroy(&editor->fonts);
    free(editor->existing_id);
    nc_string_list_free(&editor->existing_names);
    free(editor);
    return ok;
}
