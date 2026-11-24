//! 主窗口布局（对应原 `MainForm`）。

use std::collections::HashMap;

use windows_sys::Win32::Foundation::*;

use crate::ui::ids;
use crate::ui::widgets::{
    self, Align, FontRole, Fonts, Widget, ACCENT, BORDER, CARD, GRAY, GREEN, SUBTITLE_TEXT,
    SURFACE, TEXT_BUTTON, TEXT_DARK, TEXT_MUTED, WARNING_TEXT,
};
use crate::win;

pub const CLIENT_WIDTH: i32 = 560;
pub const CLIENT_HEIGHT: i32 = 574;

#[derive(Clone, Copy)]
pub struct MainControls {
    pub header: HWND,
    pub status_card: HWND,
    pub stats_card: HWND,
    pub profile_combo: HWND,
    pub new_button: HWND,
    pub edit_button: HWND,
    pub delete_button: HWND,
    pub process_box: HWND,
    pub process_button: HWND,
    pub toggle_button: HWND,
    pub hotkey_label: HWND,
    pub settings_link: HWND,
    pub uplink_stats: HWND,
    pub downlink_stats: HWND,
    pub status_dot: HWND,
    pub status_label: HWND,
    pub status_detail: HWND,
    pub minimize_button: HWND,
    pub exit_button: HWND,
}

impl Default for MainControls {
    fn default() -> Self {
        Self {
            header: std::ptr::null_mut(),
            status_card: std::ptr::null_mut(),
            stats_card: std::ptr::null_mut(),
            profile_combo: std::ptr::null_mut(),
            new_button: std::ptr::null_mut(),
            edit_button: std::ptr::null_mut(),
            delete_button: std::ptr::null_mut(),
            process_box: std::ptr::null_mut(),
            process_button: std::ptr::null_mut(),
            toggle_button: std::ptr::null_mut(),
            hotkey_label: std::ptr::null_mut(),
            settings_link: std::ptr::null_mut(),
            uplink_stats: std::ptr::null_mut(),
            downlink_stats: std::ptr::null_mut(),
            status_dot: std::ptr::null_mut(),
            status_label: std::ptr::null_mut(),
            status_detail: std::ptr::null_mut(),
            minimize_button: std::ptr::null_mut(),
            exit_button: std::ptr::null_mut(),
        }
    }
}

pub struct MainWindow {
    pub hwnd: HWND,
    pub dpi: u32,
    pub scale: f64,
    pub controls: MainControls,
}

impl MainWindow {
    pub fn create_window() -> HWND {
        win::create_window(
            "NCMain",
            "弱网模拟器",
            win::STYLE_MAIN,
            0,
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
        )
    }

    pub fn new(hwnd: HWND) -> Self {
        let dpi = win::dpi_of(hwnd);
        Self {
            hwnd,
            dpi,
            scale: dpi as f64 / 96.0,
            controls: MainControls::default(),
        }
    }

    pub fn build(&mut self, widgets_map: &mut HashMap<isize, Widget>, fonts: &Fonts) {
        let s = |value: i32| (value as f64 * self.scale).round() as i32;
        let parent = self.hwnd;
        let mut c = MainControls::default();

        c.header = widgets::create_panel(
            parent,
            ids::MAIN_HEADER,
            0,
            0,
            s(560),
            s(92),
            ACCENT,
            None,
            widgets_map,
        );
        widgets::create_text(
            parent,
            ids::MAIN_TITLE,
            "弱网模拟器",
            s(20),
            s(15),
            s(400),
            s(32),
            FontRole::Header,
            0x00FF_FFFF,
            ACCENT,
            Align::Left,
            false,
            widgets_map,
        );
        widgets::create_text(
            parent,
            ids::MAIN_SUBTITLE,
            "系统级上行 / 下行弱网控制 · 默认热键 Ctrl + Alt + U",
            s(22),
            s(54),
            s(500),
            s(20),
            FontRole::Subtitle,
            SUBTITLE_TEXT,
            ACCENT,
            Align::Left,
            false,
            widgets_map,
        );

        c.status_card = widgets::create_panel(
            parent,
            ids::MAIN_STATUS_CARD,
            s(20),
            s(112),
            s(520),
            s(92),
            CARD,
            Some(BORDER),
            widgets_map,
        );
        c.status_dot = widgets::create_text_full(
            parent,
            ids::MAIN_STATUS_DOT,
            "●",
            s(28),
            s(120),
            s(62),
            s(70),
            FontRole::Dot,
            GRAY,
            CARD,
            Align::Center,
            false,
            true,
            widgets_map,
        );
        c.status_label = widgets::create_text(
            parent,
            ids::MAIN_STATUS_LABEL,
            "未连接",
            s(98),
            s(128),
            s(425),
            s(31),
            FontRole::Status,
            TEXT_DARK,
            CARD,
            Align::Left,
            false,
            widgets_map,
        );
        c.status_detail = widgets::create_text(
            parent,
            ids::MAIN_STATUS_DETAIL,
            "网络正常，未应用任何弱网参数",
            s(100),
            s(162),
            s(420),
            s(26),
            FontRole::Small85,
            TEXT_MUTED,
            CARD,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_text(
            parent,
            ids::MAIN_PROFILE_LABEL,
            "当前配置",
            s(20),
            s(218),
            s(120),
            s(20),
            FontRole::BodyBold,
            TEXT_DARK,
            SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        c.profile_combo = widgets::create_combo(
            parent,
            ids::MAIN_PROFILE_COMBO,
            s(20),
            s(240),
            s(322),
            s(200),
        );
        c.new_button = widgets::create_button(
            parent,
            ids::MAIN_NEW,
            "新建",
            s(350),
            s(239),
            s(56),
            s(28),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            GRAY,
            Some(BORDER),
            false,
            widgets_map,
        );
        c.edit_button = widgets::create_button(
            parent,
            ids::MAIN_EDIT,
            "编辑",
            s(412),
            s(239),
            s(56),
            s(28),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            GRAY,
            Some(BORDER),
            false,
            widgets_map,
        );
        c.delete_button = widgets::create_button(
            parent,
            ids::MAIN_DELETE,
            "删除",
            s(474),
            s(239),
            s(56),
            s(28),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            GRAY,
            Some(BORDER),
            false,
            widgets_map,
        );

        widgets::create_text(
            parent,
            ids::MAIN_PROCESS_LABEL,
            "目标进程",
            s(20),
            s(284),
            s(64),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        c.process_box = widgets::create_edit(
            parent,
            ids::MAIN_PROCESS_BOX,
            "",
            s(84),
            s(278),
            s(348),
            s(27),
            true,
            Align::Left,
            None,
        );
        c.process_button = widgets::create_button(
            parent,
            ids::MAIN_PROCESS_BUTTON,
            "选择进程…",
            s(440),
            s(277),
            s(100),
            s(30),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            GRAY,
            Some(BORDER),
            false,
            widgets_map,
        );

        c.toggle_button = widgets::create_button(
            parent,
            ids::MAIN_TOGGLE,
            "启动弱网",
            s(20),
            s(318),
            s(520),
            s(50),
            GREEN,
            0x00FF_FFFF,
            FontRole::Toggle,
            GRAY,
            None,
            false,
            widgets_map,
        );

        widgets::create_text(
            parent,
            ids::MAIN_HOTKEY_TITLE,
            "全局快捷键",
            s(20),
            s(384),
            s(80),
            s(20),
            FontRole::Body,
            TEXT_MUTED,
            SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        c.hotkey_label = widgets::create_text(
            parent,
            ids::MAIN_HOTKEY_LABEL,
            "Ctrl + Alt + U",
            s(100),
            s(384),
            s(240),
            s(20),
            FontRole::BodyBold,
            TEXT_DARK,
            SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        c.settings_link = widgets::create_button(
            parent,
            ids::MAIN_SETTINGS,
            "设置",
            s(486),
            s(382),
            s(54),
            s(24),
            SURFACE,
            ACCENT,
            FontRole::Link,
            SURFACE,
            None,
            true,
            widgets_map,
        );

        c.stats_card = widgets::create_panel(
            parent,
            ids::MAIN_STATS_CARD,
            s(20),
            s(414),
            s(520),
            s(58),
            CARD,
            Some(BORDER),
            widgets_map,
        );
        c.uplink_stats = widgets::create_text(
            parent,
            ids::MAIN_UPLINK_STATS,
            "上行   发送 0   丢弃 0   队列 0",
            s(32),
            s(422),
            s(480),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            CARD,
            Align::Left,
            false,
            widgets_map,
        );
        c.downlink_stats = widgets::create_text(
            parent,
            ids::MAIN_DOWNLINK_STATS,
            "下行   发送 0   丢弃 0   队列 0",
            s(32),
            s(445),
            s(480),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            CARD,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_text(
            parent,
            ids::MAIN_HINT,
            "提示：上行丢包 / 限速也会影响下载 ACK；停止或退出程序会立即恢复网络。",
            s(20),
            s(482),
            s(520),
            s(28),
            FontRole::Small,
            WARNING_TEXT,
            SURFACE,
            Align::Left,
            true,
            widgets_map,
        );

        c.minimize_button = widgets::create_button(
            parent,
            ids::MAIN_MINIMIZE,
            "最小化到托盘",
            s(20),
            s(520),
            s(140),
            s(30),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            GRAY,
            Some(BORDER),
            false,
            widgets_map,
        );
        c.exit_button = widgets::create_button(
            parent,
            ids::MAIN_EXIT,
            "退出",
            s(400),
            s(520),
            s(140),
            s(30),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            GRAY,
            Some(BORDER),
            false,
            widgets_map,
        );

        let _ = fonts;
        self.controls = c;
        win::set_window_client_size(parent, s(CLIENT_WIDTH), s(CLIENT_HEIGHT));
        win::center_on_screen(parent);
    }

    pub fn apply_fonts(&self, fonts: &Fonts) {
        let body = fonts.get(FontRole::Body);
        for control in [self.controls.profile_combo, self.controls.process_box] {
            if !control.is_null() {
                win::set_font(control, body);
            }
        }
    }
}
