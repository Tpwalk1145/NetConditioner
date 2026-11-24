//! 系统托盘：图标、提示气泡与右键菜单。

use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::UI::Shell::{
    Shell_NotifyIconW, NIF_ICON, NIF_INFO, NIF_MESSAGE, NIF_TIP, NIM_ADD, NIM_DELETE, NIM_MODIFY,
    NOTIFYICONDATAW,
};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::ui::widgets::ORANGE;
use crate::win;

const TRAY_ID: u32 = 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrayCommand {
    Show,
    Toggle,
    Settings,
    Exit,
}

pub struct Tray {
    data: NOTIFYICONDATAW,
    normal: HICON,
    active: HICON,
    added: bool,
}

impl Tray {
    pub fn new(hwnd: HWND) -> Self {
        let normal = win::create_circle_icon(win::rgb(76, 175, 80), 32);
        let active = win::create_circle_icon(ORANGE, 32);

        let mut data: NOTIFYICONDATAW = unsafe { std::mem::zeroed() };
        data.cbSize = std::mem::size_of::<NOTIFYICONDATAW>() as u32;
        data.hWnd = hwnd;
        data.uID = TRAY_ID;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        data.uCallbackMessage = win::WM_APP_TRAY;
        data.hIcon = normal;
        win::write_fixed(&mut data.szTip, "弱网模拟器 - 未连接");

        let added = unsafe { Shell_NotifyIconW(NIM_ADD, &data) } != 0;
        Self {
            data,
            normal,
            active,
            added,
        }
    }

    pub fn set_running(&mut self, running: bool) {
        self.data.hIcon = if running { self.active } else { self.normal };
        win::write_fixed(
            &mut self.data.szTip,
            if running {
                "弱网模拟器 - 已连接"
            } else {
                "弱网模拟器 - 未连接"
            },
        );
        self.data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        unsafe {
            Shell_NotifyIconW(NIM_MODIFY, &self.data);
        }
    }

    pub fn balloon(&mut self, title: &str, text: &str, flags: u32) {
        let mut data = self.data;
        data.uFlags |= NIF_INFO;
        data.dwInfoFlags = flags;
        win::write_fixed(&mut data.szInfoTitle, title);
        win::write_fixed(&mut data.szInfo, text);
        unsafe {
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }
    }

    /// 处理托盘图标的鼠标回调。
    pub fn handle_message(&mut self, lparam: isize, running: bool) -> Option<TrayCommand> {
        const WM_LBUTTONUP: u32 = 0x0202;
        const WM_LBUTTONDBLCLK: u32 = 0x0203;
        const WM_RBUTTONUP: u32 = 0x0205;

        let message = lparam as u32;
        match message {
            WM_LBUTTONDBLCLK | WM_LBUTTONUP => Some(TrayCommand::Show),
            WM_RBUTTONUP => self.show_menu(running),
            _ => None,
        }
    }

    fn show_menu(&mut self, running: bool) -> Option<TrayCommand> {
        unsafe {
            let menu = CreatePopupMenu();
            if menu.is_null() {
                return None;
            }

            let show = win::wide("显示主窗口");
            AppendMenuW(menu, MF_STRING, 1, show.as_ptr());
            let toggle_text = win::wide(if running {
                "停止并恢复"
            } else {
                "启动弱网"
            });
            AppendMenuW(menu, MF_STRING, 2, toggle_text.as_ptr());
            AppendMenuW(menu, MF_SEPARATOR, 0, std::ptr::null());
            let settings = win::wide("设置");
            AppendMenuW(menu, MF_STRING, 3, settings.as_ptr());
            AppendMenuW(menu, MF_SEPARATOR, 0, std::ptr::null());
            let exit = win::wide("退出");
            AppendMenuW(menu, MF_STRING, 4, exit.as_ptr());

            win::set_foreground(self.data.hWnd);
            let (x, y) = win::cursor_pos();
            let command = TrackPopupMenu(
                menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                x,
                y,
                0,
                self.data.hWnd,
                std::ptr::null(),
            );
            win::post(self.data.hWnd, WM_NULL);
            DestroyMenu(menu);

            match command {
                1 => Some(TrayCommand::Show),
                2 => Some(TrayCommand::Toggle),
                3 => Some(TrayCommand::Settings),
                4 => Some(TrayCommand::Exit),
                _ => None,
            }
        }
    }

    pub fn destroy(&mut self) {
        if self.added {
            unsafe {
                Shell_NotifyIconW(NIM_DELETE, &self.data);
            }
            self.added = false;
        }
        win::destroy_icon(self.normal);
        win::destroy_icon(self.active);
        self.normal = std::ptr::null_mut();
        self.active = std::ptr::null_mut();
    }
}
