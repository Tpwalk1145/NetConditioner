//! WinDivert 许可文本导出。

use windows_sys::Win32::UI::WindowsAndMessaging::{MB_ICONINFORMATION, MB_OK};

const WINDIVERT_LICENSE: &[u8] = include_bytes!("../third_party/WinDivert/LICENSE");
static WINDIVERT_VERSION: &str = include_str!("../third_party/WinDivert/VERSION");

pub fn run() -> i32 {
    let base = crate::config::store::absolute_env_dir("TEMP").unwrap_or_else(std::env::temp_dir);
    let directory = base.join("NetConditioner").join("licenses");
    let _ = std::fs::create_dir_all(&directory);

    let path = directory.join(format!("WinDivert-LICENSE-{}.txt", crate::config::new_id()));
    let mut file = match std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
    {
        Ok(file) => file,
        Err(error) => {
            crate::win::message_box(
                std::ptr::null_mut(),
                &format!("写入许可文本失败：{error}"),
                "NetConditioner 许可",
                MB_OK | MB_ICONINFORMATION,
            );
            return 1;
        }
    };
    if let Err(error) =
        std::io::Write::write_all(&mut file, WINDIVERT_LICENSE).and_then(|_| file.sync_all())
    {
        let _ = std::fs::remove_file(&path);
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
