#include "settings.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "hotkey.h"
#include "neturl.h"
#include "ui/ids.h"
#include "version.h"
#include "win.h"

struct NcSettingsDialog {
    HWND hwnd;
    bool closed;
    NcHotkey candidate;
    HWND hotkey_edit;
    HWND hint_label;
    NcNumberInput max_run;
    NcNumberInput max_queue;
    HWND auto_update;
    HWND feed_edit;
    NcFonts fonts;
    bool has_outcome;
    NcSettingsOutcome outcome;
};

static int scale_of(int value, double scale) {
    return (int)((double)value * scale + 0.5);
}

static LRESULT CALLBACK hotkey_edit_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    WNDPROC old_proc = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    if (message == WM_CHAR) {
        return 0;
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        int virtual_key = (int)wparam;
        uint32_t modifiers = 0;
        if (virtual_key == VK_ESCAPE) {
            nc_app_settings_request_cancel(NULL);
            return 0;
        }
        if (virtual_key == VK_RETURN) {
            nc_app_settings_request_confirm(NULL);
            return 0;
        }
        if (nc_hotkey_is_modifier_key(virtual_key)) {
            return 0;
        }
        if (nc_key_down(VK_CONTROL)) {
            modifiers |= NC_MOD_CONTROL;
        }
        if (nc_key_down(VK_MENU)) {
            modifiers |= NC_MOD_ALT;
        }
        if (nc_key_down(VK_SHIFT)) {
            modifiers |= NC_MOD_SHIFT;
        }
        if (nc_key_down(VK_LWIN) || nc_key_down(VK_RWIN)) {
            modifiers |= NC_MOD_WIN;
        }
        nc_app_settings_hotkey_pressed(NULL, modifiers, virtual_key);
        return 0;
    }
    if (!old_proc) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return CallWindowProcW(old_proc, hwnd, message, wparam, lparam);
}

NcSettingsDialog *nc_settings_dialog_create(NcApp *app) {
    NcSettingsDialog *dialog;
    HWND owner;
    double preferred;
    double scale;
    HWND parent;
    NcConfig *config;
    WNDPROC old_proc;
    if (!app || !nc_app_config(app)) {
        return NULL;
    }
    config = nc_app_config(app);
    owner = nc_app_main_hwnd(app);
    preferred = nc_app_scale(app);
    if (preferred <= 0) {
        preferred = 1.0;
    }
    scale = nc_fit_dialog_scale(owner, 500, 520, preferred);
    dialog = (NcSettingsDialog *)calloc(1, sizeof(NcSettingsDialog));
    if (!dialog) {
        return NULL;
    }
    dialog->candidate = config->hotkey;
    nc_fonts_init(&dialog->fonts, (UINT)(scale * 96.0 + 0.5));
    dialog->hwnd = nc_create_window(L"NCSettings", L"设置", NC_STYLE_DIALOG, WS_EX_CONTROLPARENT,
                                    owner, 0, NULL);
    if (!dialog->hwnd) {
        nc_settings_dialog_free(dialog);
        return NULL;
    }
    nc_set_window_client_size(dialog->hwnd, scale_of(500, scale), scale_of(520, scale));
    nc_center_on_owner(dialog->hwnd, owner);
    parent = dialog->hwnd;
#define S(v) scale_of((v), scale)
    nc_widget_create_panel(nc_app_widgets(app), parent, NC_ID_SETTINGS_HEADER, 0, 0, S(500), S(58),
                           NC_COLOR_ACCENT, false, 0);
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_TITLE, "设置", S(20), S(14),
                          S(300), S(30), NC_FONT_TITLE, 0x00FFFFFF, NC_COLOR_ACCENT,
                          NC_ALIGN_LEFT, false, false);
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_HOTKEY_LABEL, "全局快捷键",
                          S(20), S(82), S(200), S(20), NC_FONT_BODY, NC_COLOR_TEXT_DARK,
                          NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
    dialog->hotkey_edit = nc_widget_create_edit(parent, NC_ID_SETTINGS_HOTKEY_EDIT, "", S(20),
                                                S(104), S(460), S(32), true, NC_ALIGN_CENTER, NULL);
    nc_set_font(dialog->hotkey_edit, nc_fonts_get(&dialog->fonts, NC_FONT_HOTKEY));
    {
        char text[64];
        nc_hotkey_format(dialog->candidate.modifiers, dialog->candidate.key, text, sizeof(text));
        nc_set_text(dialog->hotkey_edit, text);
    }
    old_proc = (WNDPROC)SetWindowLongPtrW(dialog->hotkey_edit, GWLP_WNDPROC,
                                         (LONG_PTR)hotkey_edit_proc);
    if (old_proc) {
        SetWindowLongPtrW(dialog->hotkey_edit, GWLP_USERDATA, (LONG_PTR)old_proc);
    }
    dialog->hint_label = nc_widget_create_text(
        nc_app_widgets(app), parent, NC_ID_SETTINGS_HOTKEY_HINT,
        "可直接按单键（如 F8），也可加 Ctrl/Alt/Shift/Win；单键会全局占用该键。", S(20), S(140),
        S(460), S(22), NC_FONT_SMALL, NC_COLOR_TEXT_MUTED, NC_COLOR_SURFACE, NC_ALIGN_LEFT, false,
        false);
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_MAXRUN_LABEL,
                          "最长运行时间（分钟，0 = 不自动停止）", S(20), S(180), S(300), S(20),
                          NC_FONT_BODY, NC_COLOR_TEXT_DARK, NC_COLOR_SURFACE, NC_ALIGN_LEFT, false,
                          false);
    nc_number_create(&dialog->max_run, parent, NC_ID_SETTINGS_MAXRUN_NUMBER, S(280), S(176),
                     S(120), S(24), 1440, 5);
    nc_set_font(dialog->max_run.edit, nc_fonts_get(&dialog->fonts, NC_FONT_BODY));
    nc_number_set(&dialog->max_run, config->settings.max_run_minutes);
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_MAXRUN_HINT,
                          "用于远程桌面等可能被断线的场景：到时自动停止并恢复网络。", S(20), S(204),
                          S(460), S(20), NC_FONT_SMALL, NC_COLOR_TEXT_MUTED, NC_COLOR_SURFACE,
                          NC_ALIGN_LEFT, false, false);
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_MAXQUEUE_LABEL,
                          "最大额外排队时延（ms，0 = 不限制）", S(20), S(232), S(300), S(20),
                          NC_FONT_BODY, NC_COLOR_TEXT_DARK, NC_COLOR_SURFACE, NC_ALIGN_LEFT, false,
                          false);
    nc_number_create(&dialog->max_queue, parent, NC_ID_SETTINGS_MAXQUEUE_NUMBER, S(280), S(228),
                     S(120), S(24), 60000, 500);
    nc_set_font(dialog->max_queue.edit, nc_fonts_get(&dialog->fonts, NC_FONT_BODY));
    nc_number_set(&dialog->max_queue, config->settings.max_queue_delay_ms);
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_MAXQUEUE_HINT,
                          "额外排队上限；越小越不易卡顿，越大越不易丢包。修改后需重启弱网。", S(20),
                          S(258), S(460), S(20), NC_FONT_SMALL, NC_COLOR_TEXT_MUTED,
                          NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_UPDATE_TITLE, "软件更新",
                          S(20), S(290), S(200), S(20), NC_FONT_BODY_BOLD, NC_COLOR_TEXT_DARK,
                          NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
    {
        wchar_t *wide = nc_wide_dup("自动静默更新（后台下载并重启，不提示）");
        dialog->auto_update = nc_create_child(parent, L"BUTTON", wide,
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                              0, S(20), S(314), S(460), S(22),
                                              NC_ID_SETTINGS_AUTOUPDATE);
        free(wide);
    }
    nc_set_font(dialog->auto_update, nc_fonts_get(&dialog->fonts, NC_FONT_BODY));
    SendMessageW(dialog->auto_update, BM_SETCHECK,
                 config->settings.auto_update ? BST_CHECKED : BST_UNCHECKED, 0);
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_FEED_LABEL,
                          "更新地址（latest.json）", S(20), S(346), S(200), S(20), NC_FONT_BODY,
                          NC_COLOR_TEXT_DARK, NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
    dialog->feed_edit = nc_widget_create_edit(parent, NC_ID_SETTINGS_FEED_EDIT,
                                              config->settings.update_feed_url, S(20), S(368),
                                              S(460), S(27), false, NC_ALIGN_LEFT,
                                              "https://…/latest.json");
    nc_set_font(dialog->feed_edit, nc_fonts_get(&dialog->fonts, NC_FONT_BODY));
    nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_FEED_HINT,
                          "留空 = 关闭云更新；仅支持 https（本机回环地址可用 http 调试）。", S(20),
                          S(398), S(460), S(20), NC_FONT_SMALL, NC_COLOR_TEXT_MUTED,
                          NC_COLOR_SURFACE, NC_ALIGN_LEFT, false, false);
    {
        char version_text[64];
        Str label;
        nc_version_display(version_text, sizeof(version_text));
        str_init(&label);
        str_addf(&label, "当前版本：%s", version_text);
        nc_widget_create_text(nc_app_widgets(app), parent, NC_ID_SETTINGS_VERSION,
                              str_cstr(&label), S(20), S(424), S(300), S(20), NC_FONT_SMALL,
                              NC_COLOR_TEXT_MUTED, NC_COLOR_SURFACE, NC_ALIGN_LEFT, false,
                              false);
        str_free(&label);
    }
    nc_widget_create_button(nc_app_widgets(app), parent, NC_ID_SETTINGS_CANCEL, "取消", S(260),
                            S(462), S(100), S(30), NC_COLOR_CARD, NC_COLOR_TEXT_BUTTON,
                            NC_FONT_BODY, NC_COLOR_GRAY, true, NC_COLOR_BORDER, false);
    nc_widget_create_button(nc_app_widgets(app), parent, NC_ID_SETTINGS_OK, "保存", S(370),
                            S(462), S(110), S(30), NC_COLOR_ACCENT, 0x00FFFFFF,
                            NC_FONT_BODY_BOLD, NC_COLOR_GRAY, false, 0, false);
#undef S
    nc_widget_apply_fonts(nc_app_widgets(app), &dialog->fonts);
    return dialog;
}

HWND nc_settings_dialog_hwnd(const NcSettingsDialog *dialog) {
    return dialog ? dialog->hwnd : NULL;
}

const NcFonts *nc_settings_dialog_fonts(const NcSettingsDialog *dialog) {
    return dialog ? &dialog->fonts : NULL;
}

bool nc_settings_dialog_closed(const NcSettingsDialog *dialog) {
    return dialog ? dialog->closed : true;
}

void nc_settings_dialog_free(NcSettingsDialog *dialog) {
    if (!dialog) {
        return;
    }
    nc_fonts_destroy(&dialog->fonts);
    free(dialog->outcome.update_feed_url);
    free(dialog);
}

bool nc_settings_dialog_take_outcome(NcSettingsDialog *dialog, NcSettingsOutcome *out) {
    if (!dialog || !out || !dialog->has_outcome) {
        return false;
    }
    *out = dialog->outcome;
    memset(&dialog->outcome, 0, sizeof(dialog->outcome));
    dialog->has_outcome = false;
    return true;
}

void nc_settings_cancel(NcSettingsDialog *dialog, NcApp *app) {
    if (!dialog) {
        return;
    }
    dialog->closed = true;
    if (app) {
        nc_app_cleanup_dialog_widgets(app, dialog->hwnd);
    }
    nc_destroy(dialog->hwnd);
}

void nc_settings_set_candidate(NcSettingsDialog *dialog, NcApp *app, uint32_t modifiers,
                               int32_t key) {
    char text[64];
    if (!dialog || !app) {
        return;
    }
    if (!nc_hotkey_is_valid(modifiers, key)) {
        nc_widget_set_text(nc_app_widgets(app), dialog->hint_label,
                           "请按一个普通按键（不能只按修饰键）；单键会全局占用该键。");
        nc_widget_set_text_color(nc_app_widgets(app), dialog->hint_label, NC_COLOR_RED);
        return;
    }
    dialog->candidate.modifiers = modifiers;
    dialog->candidate.key = key;
    nc_hotkey_format(modifiers, key, text, sizeof(text));
    nc_set_text(dialog->hotkey_edit, text);
    nc_widget_set_text(nc_app_widgets(app), dialog->hint_label,
                       "可直接按单键（如 F8），也可加 Ctrl/Alt/Shift/Win；单键会全局占用该键。");
    nc_widget_set_text_color(nc_app_widgets(app), dialog->hint_label, NC_COLOR_TEXT_MUTED);
}

void nc_settings_confirm(NcSettingsDialog *dialog, NcApp *app) {
    char *raw_feed;
    char *feed;
    NcSettingsOutcome outcome;
    NcErr error;
    bool auto_update;
    if (!dialog || !app) {
        return;
    }
    if (!nc_hotkey_is_valid(dialog->candidate.modifiers, dialog->candidate.key)) {
        nc_app_show_message(app, "请先设置一个有效的快捷键。", "设置", MB_OK | MB_ICONWARNING);
        return;
    }
    raw_feed = nc_get_text(dialog->feed_edit);
    feed = raw_feed ? nc_trim(raw_feed) : NULL;
    if (feed && feed[0] && !nc_url_validate_feed(feed, &error)) {
        nc_app_show_message(app, error.msg, "设置", MB_OK | MB_ICONWARNING);
        free(raw_feed);
        return;
    }
    if (!nc_number_valid(&dialog->max_run)) {
        nc_app_show_message(app, "请输入有效的非负整数。", "设置无效", MB_OK | MB_ICONWARNING);
        nc_set_focus(dialog->max_run.edit);
        free(raw_feed);
        return;
    }
    if (!nc_number_valid(&dialog->max_queue)) {
        nc_app_show_message(app, "请输入有效的非负整数。", "设置无效", MB_OK | MB_ICONWARNING);
        nc_set_focus(dialog->max_queue.edit);
        free(raw_feed);
        return;
    }
    auto_update = SendMessageW(dialog->auto_update, BM_GETCHECK, 0, 0) == BST_CHECKED;
    memset(&outcome, 0, sizeof(outcome));
    outcome.modifiers = dialog->candidate.modifiers;
    outcome.key = dialog->candidate.key;
    outcome.max_run_minutes = nc_number_value(&dialog->max_run);
    outcome.max_queue_delay_ms = nc_number_value(&dialog->max_queue);
    outcome.auto_update = auto_update;
    outcome.update_feed_url = nc_strdup(feed ? feed : "");
    free(raw_feed);
    free(dialog->outcome.update_feed_url);
    dialog->outcome = outcome;
    dialog->has_outcome = true;
    dialog->closed = true;
    nc_app_cleanup_dialog_widgets(app, dialog->hwnd);
    nc_destroy(dialog->hwnd);
}

bool nc_settings_dialog_message(NcSettingsDialog *dialog, NcApp *app, UINT message, WPARAM wparam,
                                LPARAM lparam, LRESULT *result) {
    if (!dialog) {
        return false;
    }
    switch (message) {
    case WM_ERASEBKGND:
        *result = 1;
        return true;
    case WM_PAINT:
        *result = nc_app_paint_surface(app, dialog->hwnd);
        return true;
    case WM_DRAWITEM:
        *result = nc_app_draw_item(app, lparam);
        return true;
    case WM_CTLCOLORSTATIC:
        nc_set_bk_color((HDC)wparam, NC_COLOR_SURFACE);
        nc_set_text_color((HDC)wparam, NC_COLOR_TEXT_DARK);
        *result = (LRESULT)nc_app_white_brush(app);
        return true;
    case WM_CTLCOLOREDIT:
        nc_set_bk_color((HDC)wparam, NC_COLOR_CARD);
        nc_set_text_color((HDC)wparam, NC_COLOR_TEXT_DARK);
        *result = (LRESULT)nc_app_white_brush(app);
        return true;
    case WM_CTLCOLORBTN: {
        uint32_t background;
        uint32_t foreground;
        if (nc_app_button_colors(app, (HWND)lparam, &background, &foreground)) {
            nc_set_dc_brush_color((HDC)wparam, background);
            nc_set_text_color((HDC)wparam, foreground);
            *result = (LRESULT)nc_dc_brush();
            return true;
        }
        return false;
    }
    case WM_COMMAND: {
        int id = (int)LOWORD(wparam);
        UINT code = HIWORD(wparam);
        if (id == NC_ID_SETTINGS_OK) {
            nc_settings_confirm(dialog, app);
        } else if (id == NC_ID_SETTINGS_CANCEL) {
            nc_settings_cancel(dialog, app);
        } else if (code == EN_KILLFOCUS && id == NC_ID_SETTINGS_MAXRUN_NUMBER) {
            nc_number_clamp(&dialog->max_run);
        } else if (code == EN_KILLFOCUS && id == NC_ID_SETTINGS_MAXQUEUE_NUMBER) {
            nc_number_clamp(&dialog->max_queue);
        }
        *result = 0;
        return true;
    }
    case WM_NOTIFY:
        if (lparam) {
            NMHDR *header = (NMHDR *)lparam;
            if (header->code == UDN_DELTAPOS) {
                NMUPDOWN *updown = (NMUPDOWN *)lparam;
                UINT_PTR id = header->idFrom;
                if (id == (UINT_PTR)(NC_ID_SETTINGS_MAXRUN_NUMBER + 5000)) {
                    nc_number_delta(&dialog->max_run, updown->iDelta);
                } else if (id == (UINT_PTR)(NC_ID_SETTINGS_MAXQUEUE_NUMBER + 5000)) {
                    nc_number_delta(&dialog->max_queue, updown->iDelta);
                }
                *result = 1;
                return true;
            }
        }
        *result = 0;
        return true;
    case WM_CLOSE:
        nc_settings_cancel(dialog, app);
        *result = 0;
        return true;
    case WM_DESTROY:
        dialog->closed = true;
        *result = 0;
        return true;
    default:
        return false;
    }
}