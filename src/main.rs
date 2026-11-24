#![windows_subsystem = "windows"]

mod app;
mod checkupdate;
mod config;
mod engine;
mod hotkey;
mod neturl;
mod selftest;
mod ui;
mod update;
mod version;
mod win;

use windows_sys::Win32::UI::WindowsAndMessaging::{
    MB_ICONERROR, MB_ICONINFORMATION, MB_ICONWARNING, MB_OK,
};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let has_flag = |flag: &str| args.iter().any(|arg| arg.eq_ignore_ascii_case(flag));

    if has_flag("--self-test") {
        std::process::exit(selftest::run());
    }
    if has_flag("--check-update") {
        std::process::exit(checkupdate::run());
    }

    win::set_dpi_awareness();
    win::init_common_controls();

    let mutex = win::create_single_instance_mutex();
    if mutex.is_none() {
        win::message_box(
            std::ptr::null_mut(),
            "弱网模拟器已经在运行。",
            "NetConditioner",
            MB_OK | MB_ICONINFORMATION,
        );
        return;
    }
    let _mutex = mutex;

    if !win::is_elevated() {
        win::message_box(
            std::ptr::null_mut(),
            "WinDivert 需要管理员权限。请右键选择“以管理员身份运行”。",
            "NetConditioner",
            MB_OK | MB_ICONWARNING,
        );
        return;
    }

    std::panic::set_hook(Box::new(|info| {
        let message = format!("发生未知错误。\r\n\r\n{info}");
        update::log(&format!("未处理异常：{info}"));
        win::message_box(
            std::ptr::null_mut(),
            &message,
            "NetConditioner",
            MB_OK | MB_ICONERROR,
        );
    }));

    let code = match std::panic::catch_unwind(app::run) {
        Ok(code) => code,
        Err(_) => 1,
    };

    let _ = _mutex;
    std::process::exit(code);
}
