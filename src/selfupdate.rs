//! 单文件版自更新：把当前 exe 复制成 updater，等待父进程退出后替换目标并重启。

use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use windows_sys::Win32::Foundation::CloseHandle;
use windows_sys::Win32::System::Threading::{OpenProcess, WaitForSingleObject};

pub const APPLY_FLAG: &str = "--apply-update";
const SYNCHRONIZE: u32 = 0x0010_0000;
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

pub struct ApplyArgs {
    pub target: PathBuf,
    pub new_file: PathBuf,
    pub parent_pid: u32,
    pub restart: bool,
}

pub fn is_helper_mode(args: &[String]) -> bool {
    args.iter().any(|arg| arg.eq_ignore_ascii_case(APPLY_FLAG))
}

pub fn parse_args(args: &[String]) -> Option<ApplyArgs> {
    let value_of = |name: &str| {
        args.iter()
            .position(|arg| arg.eq_ignore_ascii_case(name))
            .and_then(|index| args.get(index + 1))
            .cloned()
    };

    let target = PathBuf::from(value_of("--target")?);
    let new_file = PathBuf::from(value_of("--new")?);
    let parent_pid = value_of("--pid")?.parse::<u32>().ok()?;
    let restart = !args
        .iter()
        .any(|arg| arg.eq_ignore_ascii_case("--no-restart"));

    Some(ApplyArgs {
        target,
        new_file,
        parent_pid,
        restart,
    })
}

/// 更新助手进程：等待旧进程退出，替换 exe，必要时重新启动。
pub fn run_helper(args: &ApplyArgs) -> i32 {
    wait_for_process(args.parent_pid, Duration::from_secs(30));

    if !args.new_file.exists() {
        crate::update::log(&format!(
            "自更新失败：更新包不存在（{}）。",
            args.new_file.display()
        ));
        return 2;
    }

    let mut copied = false;
    for attempt in 0..25 {
        match std::fs::copy(&args.new_file, &args.target) {
            Ok(_) => {
                copied = true;
                break;
            }
            Err(error) => {
                if attempt == 24 {
                    crate::update::log(&format!(
                        "自更新失败：无法替换 {}（{error}）。",
                        args.target.display()
                    ));
                } else {
                    std::thread::sleep(Duration::from_millis(200));
                }
            }
        }
    }

    if !copied {
        return 3;
    }

    remove_zone_identifier(&args.target);
    crate::update::log(&format!("自更新完成：{}", args.target.display()));

    if args.restart {
        match Command::new(&args.target).spawn() {
            Ok(_) => crate::update::log("已重新启动新版本。"),
            Err(error) => {
                crate::update::log(&format!("重新启动新版本失败：{error}"));
                return 4;
            }
        }
    }

    0
}

/// 由旧版本进程调用：复制自身为助手并启动它，然后由调用方退出。
pub fn spawn_helper(target: &Path, new_file: &Path) -> Result<(), String> {
    let current =
        std::env::current_exe().map_err(|error| format!("读取当前程序路径失败：{error}"))?;
    let directory = crate::update::update_directory();
    std::fs::create_dir_all(&directory).map_err(|error| format!("创建更新目录失败：{error}"))?;

    let helper = directory.join("NetConditioner-updater.exe");
    std::fs::copy(&current, &helper).map_err(|error| format!("准备更新程序失败：{error}"))?;

    let pid = std::process::id();
    Command::new(&helper)
        .arg(APPLY_FLAG)
        .arg("--target")
        .arg(target)
        .arg("--new")
        .arg(new_file)
        .arg("--pid")
        .arg(pid.to_string())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|error| format!("启动更新程序失败：{error}"))?;

    Ok(())
}

fn wait_for_process(pid: u32, timeout: Duration) {
    if pid == 0 {
        return;
    }

    unsafe {
        let handle = OpenProcess(SYNCHRONIZE, 0, pid);
        if handle.is_null() {
            // 进程已经退出。
            return;
        }
        let milliseconds = timeout.as_millis().min(u32::MAX as u128) as u32;
        WaitForSingleObject(handle, milliseconds);
        CloseHandle(handle);
    }
}

/// 去掉下载文件上的 Zone.Identifier 备用数据流，避免静默启动时被 SmartScreen 拦截。
pub fn remove_zone_identifier(path: &Path) {
    let mut stream = path.as_os_str().to_os_string();
    stream.push(":Zone.Identifier");
    let text = stream.to_string_lossy().to_string();
    let wide: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
    unsafe {
        windows_sys::Win32::Storage::FileSystem::DeleteFileW(wide.as_ptr());
    }
}
