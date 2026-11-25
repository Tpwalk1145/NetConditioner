//! 设置对话框。

use std::sync::atomic::{AtomicIsize, Ordering};

use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::HDC;
use windows_sys::Win32::UI::Controls::*;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    VK_CONTROL, VK_ESCAPE, VK_LWIN, VK_MENU, VK_RETURN, VK_RWIN, VK_SHIFT,
};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::app::{app, App, Dialog};
use crate::config::{HotkeySettings, MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN};
use crate::hotkey;
use crate::neturl;
use crate::ui::ids;
use crate::ui::widgets::{
    self, Align, FontRole, Fonts, NumberInput, Widget, CARD, TEXT_BUTTON, TEXT_DARK, TEXT_MUTED,
};
use crate::win;

const CLIENT_WIDTH: i32 = 500;
const CLIENT_HEIGHT: i32 = 520;

static HOTKEY_EDIT_OLD_PROC: AtomicIsize = AtomicIsize::new(0);

pub struct SettingsOutcome {
    pub hotkey: HotkeySettings,
    pub max_run_minutes: i32,
    pub max_queue_delay_ms: i32,
    pub auto_update: bool,
    pub update_feed_url: String,
}

pub struct SettingsDialog {
    pub hwnd: HWND,
    pub closed: bool,
    candidate: HotkeySettings,
    hotkey_edit: HWND,
    hint_label: HWND,
    max_run: NumberInput,
    max_queue: NumberInput,
    auto_update: HWND,
    feed_edit: HWND,
    outcome: Option<SettingsOutcome>,
}

impl SettingsDialog {
    pub fn create(app: &mut App) -> Self {
        let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
        let scale = app.main.as_ref().map(|main| main.scale).unwrap_or(1.0);
        let fonts = &app.fonts;
        let s = |value: i32| (value as f64 * scale).round() as i32;

        let hwnd = win::create_window(
            "NCSettings",
            "设置",
            win::STYLE_DIALOG,
            WS_EX_CONTROLPARENT,
            owner,
            0,
            std::ptr::null_mut(),
        );
        win::set_window_client_size(hwnd, s(CLIENT_WIDTH), s(CLIENT_HEIGHT));
        win::center_on_owner(hwnd, owner);

        let settings_snapshot = app.config.settings.clone();
        let current_hotkey = app.config.hotkey.clone();
        let widgets_map = &mut app.widgets;

        widgets::create_panel(
            hwnd,
            ids::SETTINGS_HEADER,
            0,
            0,
            s(500),
            s(58),
            widgets::ACCENT,
            None,
            widgets_map,
        );
        widgets::create_text(
            hwnd,
            ids::SETTINGS_TITLE,
            "设置",
            s(20),
            s(14),
            s(300),
            s(30),
            FontRole::Title,
            0x00FF_FFFF,
            widgets::ACCENT,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_text(
            hwnd,
            ids::SETTINGS_HOTKEY_LABEL,
            "全局快捷键",
            s(20),
            s(82),
            s(200),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        let hotkey_edit = widgets::create_edit(
            hwnd,
            ids::SETTINGS_HOTKEY_EDIT,
            "",
            s(20),
            s(104),
            s(460),
            s(32),
            true,
            Align::Center,
            None,
        );
        win::set_font(hotkey_edit, fonts.get(FontRole::Hotkey));
        win::set_text(
            hotkey_edit,
            &hotkey::format(current_hotkey.modifiers, current_hotkey.key),
        );
        // 子类化：捕获按键而不是把字符输入文本框。
        let new_proc: unsafe extern "system" fn(HWND, u32, WPARAM, LPARAM) -> LRESULT =
            hotkey_edit_proc;
        let old_proc = unsafe {
            let old = SetWindowLongPtrW(hotkey_edit, GWLP_WNDPROC, new_proc as isize);
            SetWindowLongPtrW(hotkey_edit, GWLP_USERDATA, old);
            old
        };
        HOTKEY_EDIT_OLD_PROC.store(old_proc, Ordering::Relaxed);

        let hint_label = widgets::create_text(
            hwnd,
            ids::SETTINGS_HOTKEY_HINT,
            "可直接按单键（如 F8），也可加 Ctrl/Alt/Shift/Win；单键会全局占用该键。",
            s(20),
            s(140),
            s(460),
            s(22),
            FontRole::Small,
            TEXT_MUTED,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_text(
            hwnd,
            ids::SETTINGS_MAXRUN_LABEL,
            "最长运行时间（分钟，0 = 不自动停止）",
            s(20),
            s(180),
            s(300),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        let max_run = NumberInput::create(
            hwnd,
            ids::SETTINGS_MAXRUN_NUMBER,
            s(280),
            s(176),
            s(120),
            s(24),
            1440,
            5,
        );
        win::set_font(max_run.edit, fonts.get(FontRole::Body));
        max_run.set_value(settings_snapshot.max_run_minutes.clamp(0, 1440));

        widgets::create_text(
            hwnd,
            ids::SETTINGS_MAXRUN_HINT,
            "用于远程桌面等可能被断线的场景：到时自动停止并恢复网络。",
            s(20),
            s(204),
            s(460),
            s(20),
            FontRole::Small,
            TEXT_MUTED,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_text(
            hwnd,
            ids::SETTINGS_MAXQUEUE_LABEL,
            "最大额外排队时延（ms，0 = 不限制）",
            s(20),
            s(232),
            s(300),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        let max_queue = NumberInput::create(
            hwnd,
            ids::SETTINGS_MAXQUEUE_NUMBER,
            s(280),
            s(228),
            s(120),
            s(24),
            60000,
            500,
        );
        win::set_font(max_queue.edit, fonts.get(FontRole::Body));
        max_queue.set_value(settings_snapshot.max_queue_delay_ms.clamp(0, 60000));

        widgets::create_text(
            hwnd,
            ids::SETTINGS_MAXQUEUE_HINT,
            "额外排队上限；越小越不易卡顿，越大越不易丢包。修改后需重启弱网。",
            s(20),
            s(258),
            s(460),
            s(20),
            FontRole::Small,
            TEXT_MUTED,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_text(
            hwnd,
            ids::SETTINGS_UPDATE_TITLE,
            "软件更新",
            s(20),
            s(290),
            s(200),
            s(20),
            FontRole::BodyBold,
            TEXT_DARK,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        let auto_update = win::create_child(
            hwnd,
            "BUTTON",
            "自动静默更新（后台下载并重启，不提示）",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | (BS_AUTOCHECKBOX as u32),
            0,
            s(20),
            s(314),
            s(460),
            s(22),
            ids::SETTINGS_AUTOUPDATE,
        );
        win::set_font(auto_update, fonts.get(FontRole::Body));
        win::send(
            auto_update,
            BM_SETCHECK,
            if settings_snapshot.auto_update {
                BST_CHECKED as usize
            } else {
                BST_UNCHECKED as usize
            },
            0,
        );

        widgets::create_text(
            hwnd,
            ids::SETTINGS_FEED_LABEL,
            "更新地址（latest.json）",
            s(20),
            s(346),
            s(200),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        let feed_edit = widgets::create_edit(
            hwnd,
            ids::SETTINGS_FEED_EDIT,
            &settings_snapshot.update_feed_url,
            s(20),
            s(368),
            s(460),
            s(27),
            false,
            Align::Left,
            Some("https://…/latest.json"),
        );
        win::set_font(feed_edit, fonts.get(FontRole::Body));

        widgets::create_text(
            hwnd,
            ids::SETTINGS_FEED_HINT,
            "留空 = 关闭云更新；仅支持 https（本机回环地址可用 http 调试）。",
            s(20),
            s(398),
            s(460),
            s(20),
            FontRole::Small,
            TEXT_MUTED,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_text(
            hwnd,
            ids::SETTINGS_VERSION,
            &format!("当前版本：{}", crate::version::display()),
            s(20),
            s(424),
            s(300),
            s(20),
            FontRole::Small,
            TEXT_MUTED,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_button(
            hwnd,
            ids::SETTINGS_CANCEL,
            "取消",
            s(260),
            s(462),
            s(100),
            s(30),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            widgets::GRAY,
            Some(widgets::BORDER),
            false,
            widgets_map,
        );
        widgets::create_button(
            hwnd,
            ids::SETTINGS_OK,
            "保存",
            s(370),
            s(462),
            s(110),
            s(30),
            widgets::ACCENT,
            0x00FF_FFFF,
            FontRole::BodyBold,
            widgets::GRAY,
            None,
            false,
            widgets_map,
        );

        Self {
            hwnd,
            closed: false,
            candidate: current_hotkey,
            hotkey_edit,
            hint_label,
            max_run,
            max_queue,
            auto_update,
            feed_edit,
            outcome: None,
        }
    }

    pub fn on_message(
        &mut self,
        app: &mut App,
        message: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> Option<isize> {
        match message {
            WM_ERASEBKGND => Some(1),
            WM_PAINT => Some(app.paint_surface(self.hwnd)),
            WM_DRAWITEM => Some(app.draw_item(lparam)),
            WM_CTLCOLORSTATIC => {
                win::set_bk_color(wparam as HDC, widgets::SURFACE);
                win::set_text_color(wparam as HDC, TEXT_DARK);
                Some(app.white_brush as isize)
            }
            WM_CTLCOLOREDIT => {
                win::set_bk_color(wparam as HDC, CARD);
                win::set_text_color(wparam as HDC, TEXT_DARK);
                Some(app.white_brush as isize)
            }
            WM_COMMAND => {
                let id = (wparam & 0xFFFF) as usize;
                let code = ((wparam >> 16) & 0xFFFF) as u32;
                match id {
                    ids::SETTINGS_OK => self.confirm(app),
                    ids::SETTINGS_CANCEL => self.cancel(),
                    ids::SETTINGS_MAXRUN_NUMBER | ids::SETTINGS_MAXQUEUE_NUMBER
                        if code == EN_KILLFOCUS =>
                    {
                        if id == ids::SETTINGS_MAXRUN_NUMBER {
                            self.max_run.clamp_and_set();
                        } else {
                            self.max_queue.clamp_and_set();
                        }
                    }
                    _ => {}
                }
                Some(0)
            }
            WM_NOTIFY => {
                if lparam != 0 {
                    let header = lparam as *const NMHDR;
                    if unsafe { (*header).code } == UDN_DELTAPOS {
                        let updown = lparam as *const NMUPDOWN;
                        let id = unsafe { (*header).idFrom };
                        let delta = unsafe { (*updown).iDelta };
                        if id == ids::SETTINGS_MAXRUN_NUMBER + 5000 {
                            self.max_run.handle_delta(delta);
                        } else if id == ids::SETTINGS_MAXQUEUE_NUMBER + 5000 {
                            self.max_queue.handle_delta(delta);
                        }
                        return Some(1);
                    }
                }
                Some(0)
            }
            WM_CLOSE => {
                self.cancel();
                Some(0)
            }
            WM_DESTROY => {
                self.closed = true;
                Some(0)
            }
            _ => None,
        }
    }

    pub fn set_candidate(
        &mut self,
        modifiers: u32,
        key: i32,
        widgets_map: &mut std::collections::HashMap<isize, Widget>,
        fonts: &Fonts,
    ) {
        if !hotkey::is_valid(modifiers, key) {
            widgets::set_widget_text(
                widgets_map,
                self.hint_label,
                "请按一个普通按键（不能只按修饰键）；单键会全局占用该键。",
            );
            widgets::set_widget_text_color(widgets_map, self.hint_label, widgets::RED);
            return;
        }

        self.candidate = HotkeySettings { modifiers, key };
        win::set_text(self.hotkey_edit, &hotkey::format(modifiers, key));
        widgets::set_widget_text(
            widgets_map,
            self.hint_label,
            "可直接按单键（如 F8），也可加 Ctrl/Alt/Shift/Win；单键会全局占用该键。",
        );
        widgets::set_widget_text_color(widgets_map, self.hint_label, TEXT_MUTED);
        let _ = fonts;
    }

    pub fn cancel(&mut self) {
        self.closed = true;
        win::destroy(self.hwnd);
    }

    pub fn confirm(&mut self, app: &mut App) {
        if !hotkey::is_valid(self.candidate.modifiers, self.candidate.key) {
            app.show_message("请先设置一个有效的快捷键。", "设置", MB_OK | MB_ICONWARNING);
            return;
        }

        let feed_url = win::get_text(self.feed_edit).trim().to_string();
        if !feed_url.is_empty() {
            if let Err(error) = neturl::try_validate_feed_url(&feed_url) {
                app.show_message(&error, "设置", MB_OK | MB_ICONWARNING);
                return;
            }
        }

        let auto_update = win::send(self.auto_update, BM_GETCHECK, 0, 0) as u32 == BST_CHECKED;
        self.outcome = Some(SettingsOutcome {
            hotkey: self.candidate.clone(),
            max_run_minutes: self.max_run.value(),
            max_queue_delay_ms: self.max_queue.value(),
            auto_update,
            update_feed_url: feed_url,
        });
        self.closed = true;
        win::destroy(self.hwnd);
    }
}

unsafe extern "system" fn hotkey_edit_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    let old_proc = HOTKEY_EDIT_OLD_PROC.load(Ordering::Relaxed);
    match message {
        WM_GETDLGCODE => DLGC_WANTALLKEYS as isize,
        WM_CHAR => 0,
        WM_KEYDOWN | WM_SYSKEYDOWN => {
            let virtual_key = wparam as i32;
            if virtual_key == VK_ESCAPE as i32 {
                app().settings_request_cancel();
                return 0;
            }
            if virtual_key == VK_RETURN as i32 {
                app().settings_request_confirm();
                return 0;
            }
            if hotkey::is_modifier_key(virtual_key) {
                return 0;
            }

            let mut modifiers = 0u32;
            if win::key_down(VK_CONTROL as i32) {
                modifiers |= MOD_CONTROL;
            }
            if win::key_down(VK_MENU as i32) {
                modifiers |= MOD_ALT;
            }
            if win::key_down(VK_SHIFT as i32) {
                modifiers |= MOD_SHIFT;
            }
            if win::key_down(VK_LWIN as i32) || win::key_down(VK_RWIN as i32) {
                modifiers |= MOD_WIN;
            }

            app().settings_hotkey_pressed(modifiers, virtual_key);
            0
        }
        _ => CallWindowProcW(
            std::mem::transmute::<isize, WNDPROC>(old_proc),
            hwnd,
            message,
            wparam,
            lparam,
        ),
    }
}

pub fn open(app: &mut App) -> Option<SettingsOutcome> {
    let dialog = SettingsDialog::create(app);
    let hwnd = dialog.hwnd;
    let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
    app.dialog = Some(Dialog::Settings(dialog));
    win::run_modal(owner, hwnd);

    match app.dialog.take() {
        Some(Dialog::Settings(dialog)) => dialog.outcome,
        _ => None,
    }
}
