#include "main_window.h"

#include <string.h>

#include "ids.h"
#include "win.h"

static int scale_of(int value, double scale) {
    return (int)((double)value * scale + 0.5);
}

HWND nc_main_window_create(void) {
    return nc_create_window(L"NCMain", L"弱网模拟器", NC_STYLE_MAIN, 0, NULL, 0, NULL);
}

void nc_main_window_init(NcMainWindow *window, HWND hwnd) {
    UINT dpi;
    if (!window) {
        return;
    }
    memset(window, 0, sizeof(*window));
    window->hwnd = hwnd;
    dpi = nc_dpi_of(hwnd);
    window->scale = nc_fit_dialog_scale(hwnd, NC_MAIN_CLIENT_WIDTH, NC_MAIN_CLIENT_HEIGHT,
                                        (double)dpi / 96.0);
    window->dpi = (UINT)(window->scale * 96.0 + 0.5);
}

void nc_main_window_build(NcMainWindow *window, NcWidgets *widgets,
                          [[maybe_unused]] const NcFonts *fonts) {
    HWND parent;
    NcMainControls c;
    double scale;
    if (!window || !widgets) {
        return;
    }
    parent = window->hwnd;
    scale = window->scale;
#define S(v) scale_of((v), scale)
    memset(&c, 0, sizeof(c));
    nc_widget_create_panel(widgets, parent, NC_ID_MAIN_HEADER, 0, 0, S(560), S(92),
                           NC_COLOR_ACCENT, false, 0);
    nc_widget_create_text(widgets, parent, NC_ID_MAIN_TITLE, "弱网模拟器", S(20), S(15), S(400),
                          S(32), NC_FONT_HEADER, 0x00FFFFFF, NC_COLOR_ACCENT, NC_ALIGN_LEFT,
                          false, false);
    nc_widget_create_text(widgets, parent, NC_ID_MAIN_SUBTITLE,
                          "系统级上行 / 下行弱网控制 · 默认热键 Ctrl + Alt + U", S(22), S(54),
                          S(500), S(20), NC_FONT_SUBTITLE, NC_COLOR_SUBTITLE_TEXT, NC_COLOR_ACCENT,
                          NC_ALIGN_LEFT, false, false);
    c.status_card = nc_widget_create_panel(widgets, parent, NC_ID_MAIN_STATUS_CARD, S(20), S(112),
                                           S(520), S(92), NC_COLOR_CARD, true, NC_COLOR_BORDER);
    c.status_dot = nc_widget_create_text(widgets, parent, NC_ID_MAIN_STATUS_DOT, "●", S(28),
                                         S(120), S(62), S(70), NC_FONT_DOT, NC_COLOR_GRAY,
                                         NC_COLOR_CARD, NC_ALIGN_CENTER, false, true);
    c.status_label = nc_widget_create_text(widgets, parent, NC_ID_MAIN_STATUS_LABEL, "未连接",
                                           S(98), S(128), S(425), S(31), NC_FONT_STATUS,
                                           NC_COLOR_TEXT_DARK, NC_COLOR_CARD, NC_ALIGN_LEFT, false,
                                           false);
    c.status_detail = nc_widget_create_text(widgets, parent, NC_ID_MAIN_STATUS_DETAIL,
                                            "网络正常，未应用任何弱网参数", S(100), S(162), S(420),
                                            S(26), NC_FONT_SMALL85, NC_COLOR_TEXT_MUTED,
                                            NC_COLOR_CARD, NC_ALIGN_LEFT, false, false);
    nc_widget_create_text(widgets, parent, NC_ID_MAIN_PROFILE_LABEL, "当前配置", S(20), S(218),
                          S(120), S(20), NC_FONT_BODY_BOLD, NC_COLOR_TEXT_DARK, NC_COLOR_SURFACE,
                          NC_ALIGN_LEFT, false, false);
    c.profile_combo = nc_widget_create_combo(parent, NC_ID_MAIN_PROFILE_COMBO, S(20), S(240),
                                              S(322), S(200));
    c.new_button = nc_widget_create_button(widgets, parent, NC_ID_MAIN_NEW, "新建", S(350), S(239),
                                           S(56), S(28), NC_COLOR_CARD, NC_COLOR_TEXT_BUTTON,
                                           NC_FONT_BODY, NC_COLOR_GRAY, true, NC_COLOR_BORDER, false);
    c.edit_button = nc_widget_create_button(widgets, parent, NC_ID_MAIN_EDIT, "编辑", S(412),
                                            S(239), S(56), S(28), NC_COLOR_CARD,
                                            NC_COLOR_TEXT_BUTTON, NC_FONT_BODY, NC_COLOR_GRAY, true,
                                            NC_COLOR_BORDER, false);
    c.delete_button = nc_widget_create_button(widgets, parent, NC_ID_MAIN_DELETE, "删除", S(474),
                                              S(239), S(56), S(28), NC_COLOR_CARD,
                                              NC_COLOR_TEXT_BUTTON, NC_FONT_BODY, NC_COLOR_GRAY,
                                              true, NC_COLOR_BORDER, false);
    nc_widget_create_text(widgets, parent, NC_ID_MAIN_PROCESS_LABEL, "目标进程", S(20), S(284),
                          S(64), S(20), NC_FONT_BODY, NC_COLOR_TEXT_DARK, NC_COLOR_SURFACE,
                          NC_ALIGN_LEFT, false, false);
    c.process_box = nc_widget_create_edit(parent, NC_ID_MAIN_PROCESS_BOX, "", S(84), S(278), S(348),
                                          S(27), true, NC_ALIGN_LEFT, NULL);
    c.process_button = nc_widget_create_button(widgets, parent, NC_ID_MAIN_PROCESS_BUTTON,
                                               "选择进程…", S(440), S(277), S(100), S(30),
                                               NC_COLOR_CARD, NC_COLOR_TEXT_BUTTON, NC_FONT_BODY,
                                               NC_COLOR_GRAY, true, NC_COLOR_BORDER, false);
    c.toggle_button = nc_widget_create_button(widgets, parent, NC_ID_MAIN_TOGGLE, "启动弱网",
                                              S(20), S(318), S(520), S(50), NC_COLOR_GREEN,
                                              0x00FFFFFF, NC_FONT_TOGGLE, NC_COLOR_GRAY, false, 0,
                                              false);
    nc_widget_create_text(widgets, parent, NC_ID_MAIN_HOTKEY_TITLE, "全局快捷键", S(20), S(384),
                          S(80), S(20), NC_FONT_BODY, NC_COLOR_TEXT_MUTED, NC_COLOR_SURFACE,
                          NC_ALIGN_LEFT, false, false);
    c.hotkey_label = nc_widget_create_text(widgets, parent, NC_ID_MAIN_HOTKEY_LABEL,
                                           "Ctrl + Alt + U", S(100), S(384), S(240), S(20),
                                           NC_FONT_BODY_BOLD, NC_COLOR_TEXT_DARK, NC_COLOR_SURFACE,
                                           NC_ALIGN_LEFT, false, false);
    c.settings_link = nc_widget_create_button(widgets, parent, NC_ID_MAIN_SETTINGS, "设置", S(486),
                                              S(382), S(54), S(24), NC_COLOR_SURFACE,
                                              NC_COLOR_ACCENT, NC_FONT_LINK, NC_COLOR_SURFACE,
                                              false, 0, true);
    c.stats_card = nc_widget_create_panel(widgets, parent, NC_ID_MAIN_STATS_CARD, S(20), S(414),
                                          S(520), S(58), NC_COLOR_CARD, true, NC_COLOR_BORDER);
    c.uplink_stats = nc_widget_create_text(widgets, parent, NC_ID_MAIN_UPLINK_STATS,
                                           "上行   发送 0   丢弃 0   队列 0", S(32), S(422), S(480),
                                           S(20), NC_FONT_BODY, NC_COLOR_TEXT_DARK, NC_COLOR_CARD,
                                           NC_ALIGN_LEFT, false, false);
    c.downlink_stats = nc_widget_create_text(widgets, parent, NC_ID_MAIN_DOWNLINK_STATS,
                                             "下行   发送 0   丢弃 0   队列 0", S(32), S(445),
                                             S(480), S(20), NC_FONT_BODY, NC_COLOR_TEXT_DARK,
                                             NC_COLOR_CARD, NC_ALIGN_LEFT, false, false);
    nc_widget_create_text(widgets, parent, NC_ID_MAIN_HINT,
                          "提示：上行丢包 / 限速也会影响下载 ACK；停止或退出程序会立即恢复网络。",
                          S(20), S(482), S(520), S(28), NC_FONT_SMALL, NC_COLOR_WARNING_TEXT,
                          NC_COLOR_SURFACE, NC_ALIGN_LEFT, true, false);
    c.minimize_button = nc_widget_create_button(widgets, parent, NC_ID_MAIN_MINIMIZE, "最小化到托盘",
                                                S(20), S(520), S(140), S(30), NC_COLOR_CARD,
                                                NC_COLOR_TEXT_BUTTON, NC_FONT_BODY, NC_COLOR_GRAY,
                                                true, NC_COLOR_BORDER, false);
    c.exit_button = nc_widget_create_button(widgets, parent, NC_ID_MAIN_EXIT, "退出", S(400),
                                            S(520), S(140), S(30), NC_COLOR_CARD,
                                            NC_COLOR_TEXT_BUTTON, NC_FONT_BODY, NC_COLOR_GRAY,
                                            true, NC_COLOR_BORDER, false);
#undef S
    window->controls = c;
    nc_set_window_client_size(parent, scale_of(NC_MAIN_CLIENT_WIDTH, scale),
                              scale_of(NC_MAIN_CLIENT_HEIGHT, scale));
    nc_center_on_screen(parent);
}

void nc_main_window_apply_fonts(const NcMainWindow *window, const NcFonts *fonts) {
    HFONT body;
    if (!window || !fonts) {
        return;
    }
    body = nc_fonts_get(fonts, NC_FONT_BODY);
    nc_set_font(window->controls.profile_combo, body);
    nc_set_font(window->controls.process_box, body);
}
