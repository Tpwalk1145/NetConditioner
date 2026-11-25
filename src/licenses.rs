//! WinDivert 许可文本导出。

use windows_sys::Win32::UI::WindowsAndMessaging::{MB_ICONINFORMATION, MB_OK};

const WINDIVERT_LICENSE: &[u8] = include_bytes!("../third_party/WinDivert/LICENSE");
static WINDIVERT_VERSION: &str = include_str!("../third_party/WinDivert/VERSION");

pub fn run() -> i32 {
    let directory = std::env::temp_dir().join("NetConditioner").join("licenses");
    let _ = std::fs::create_dir_all(&directory);

    let path = directory.join("WinDivert-LICENSE.txt");
    if let Err(error) = std::fs::write(&path, WINDIVERT_LICENSE) {
        crate::win::message_box(
            std::ptr::null_mut(),
            &format!("写入许可文本失败：{error}"),
            "NetConditioner 许可",
            MB_OK | MB_ICONINFORMATION,
        );
        return 1;
    }

    let notice = format!(
        "本程序动态加载随包分发的 WinDivert {}（LGPLv3 / GPLv2 双许可）。\r\n\r\n完整许可文本已保存到：\r\n{}\r\n\r\nWinDivert 源码：https://github.com/basil00/WinDivert",
        WINDIVERT_VERSION.trim(),
        path.display()
    );
    crate::win::message_box(
        std::ptr::null_mut(),
        &notice,
        "NetConditioner 许可",
        MB_OK | MB_ICONINFORMATION,
    );
    0
}
