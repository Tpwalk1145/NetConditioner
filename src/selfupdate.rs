//! 单文件版自更新：唯一助手等待父进程退出，原子替换 exe，失败时回滚并重启原版本。

use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::windows::ffi::OsStrExt;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::Duration;

use sha2::{Digest, Sha256};
use windows_sys::Win32::Foundation::{
    CloseHandle, ERROR_ACCESS_DENIED, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows_sys::Win32::Storage::FileSystem::SYNCHRONIZE;
use windows_sys::Win32::System::Threading::{
    CreateEventW, OpenEventW, OpenProcess, SetEvent, WaitForSingleObject, EVENT_MODIFY_STATE,
};

use crate::update::{create_staging_file, ensure_update_directory, hash_file};

pub const APPLY_FLAG: &str = "--apply-update";
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

pub struct ApplyArgs {
    pub target: PathBuf,
    pub new_file: PathBuf,
    pub parent_pid: u32,
    pub restart: bool,
    pub expected_sha256: String,
    pub expected_size: u64,
    pub ready_event: String,
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
    let expected_sha256 = value_of("--sha256")?;
    let expected_size = value_of("--size")?.parse::<u64>().ok()?;
    if expected_size == 0
        || crate::neturl::try_normalize_sha256(&expected_sha256).as_deref()
            != Some(&expected_sha256)
    {
        return None;
    }
    Some(ApplyArgs {
        target: PathBuf::from(value_of("--target")?),
        new_file: PathBuf::from(value_of("--new")?),
        parent_pid: value_of("--pid")?.parse::<u32>().ok()?,
        restart: !args
            .iter()
            .any(|arg| arg.eq_ignore_ascii_case("--no-restart")),
        expected_sha256,
        expected_size,
        ready_event: value_of("--ready")?,
    })
}

#[derive(Debug, PartialEq, Eq)]
enum ProcessWait {
    Exited,
    TimedOut,
    Failed(u32),
}

fn wait_for_process(pid: u32, timeout: Duration) -> ProcessWait {
    if pid == 0 {
        return ProcessWait::Failed(ERROR_ACCESS_DENIED);
    }
    unsafe {
        let handle = OpenProcess(SYNCHRONIZE, 0, pid);
        if handle.is_null() {
            return ProcessWait::Failed(
                std::io::Error::last_os_error().raw_os_error().unwrap_or(0) as u32,
            );
        }
        let result = WaitForSingleObject(handle, timeout.as_millis().min(u32::MAX as u128) as u32);
        CloseHandle(handle);
        match result {
            WAIT_OBJECT_0 => ProcessWait::Exited,
            WAIT_TIMEOUT => ProcessWait::TimedOut,
            _ => ProcessWait::Failed(
                std::io::Error::last_os_error().raw_os_error().unwrap_or(0) as u32
            ),
        }
    }
}

fn signal_ready(name: &str) {
    let name: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();
    unsafe {
        let event = OpenEventW(EVENT_MODIFY_STATE, 0, name.as_ptr());
        if !event.is_null() {
            SetEvent(event);
            CloseHandle(event);
        }
    }
}

fn fail_and_restart(args: &ApplyArgs, message: &str) -> i32 {
    crate::update::log(message);
    if args.restart {
        match Command::new(&args.target).spawn() {
            Ok(_) => {
                crate::update::log("已重新启动原版本。");
                0
            }
            Err(error) => {
                crate::update::log(&format!("重新启动原版本失败：{error}"));
                3
            }
        }
    } else {
        3
    }
}

/// 更新助手进程：校验同一源句柄复制的字节，再原子替换并保留备份。
pub fn run_helper(args: &ApplyArgs) -> i32 {
    signal_ready(&args.ready_event);
    if let Err(error) = ensure_update_directory() {
        return fail_and_restart(args, &format!("自更新失败：{error}"));
    }
    if !matches!(
        wait_for_process(args.parent_pid, Duration::from_secs(30)),
        ProcessWait::Exited
    ) {
        return fail_and_restart(args, "自更新失败：父进程未在期限内正常退出，未替换文件。");
    }
    let mut source = match File::open(&args.new_file) {
        Ok(source) => source,
        Err(error) => {
            return fail_and_restart(args, &format!("自更新失败：无法打开更新包（{error}）。"))
        }
    };
    let (source_size, source_hash) = match hash_reader(&mut source) {
        Ok(value) => value,
        Err(error) => {
            return fail_and_restart(args, &format!("自更新失败：无法校验更新包（{error}）。"))
        }
    };
    if source_size != args.expected_size || !source_hash.eq_ignore_ascii_case(&args.expected_sha256)
    {
        return fail_and_restart(args, "自更新失败：更新包 SHA256 或大小与父进程不一致。");
    }

    let (stage_path, mut stage_file) = match create_staging_file(&args.target, "nc-self-stage") {
        Ok(value) => value,
        Err(error) => {
            return fail_and_restart(args, &format!("自更新失败：无法创建暂存文件（{error}）。"))
        }
    };
    let mut backup_path = None;
    let mut replacement_attempted = false;
    let result = (|| -> Result<(), String> {
        source
            .seek(SeekFrom::Start(0))
            .map_err(|error| error.to_string())?;
        let (copied_size, copied_hash) =
            copy_and_hash(&mut source, &mut stage_file).map_err(|error| error.to_string())?;
        if copied_size != args.expected_size
            || !copied_hash.eq_ignore_ascii_case(&args.expected_sha256)
        {
            return Err("暂存文件与更新包校验值不一致。".to_string());
        }
        stage_file.sync_all().map_err(|error| error.to_string())?;
        drop(stage_file);
        let (staged_size, staged_hash) = hash_file(&stage_path)?;
        if staged_size != args.expected_size
            || !staged_hash.eq_ignore_ascii_case(&args.expected_sha256)
        {
            return Err("暂存文件复核失败。".to_string());
        }
        let (candidate_backup, mut backup_file) =
            create_staging_file(&args.target, "nc-self-backup")?;
        let mut original = File::open(&args.target).map_err(|error| error.to_string())?;
        let (_, original_hash) =
            copy_and_hash(&mut original, &mut backup_file).map_err(|error| error.to_string())?;
        let (expected_size, expected_hash) =
            hash_file(&args.target).map_err(|error| error.to_string())?;
        if original_hash != expected_hash
            || std::fs::metadata(&args.target)
                .map(|m| m.len())
                .unwrap_or(0)
                != expected_size
        {
            return Err("原程序在备份期间发生变化。".to_string());
        }
        backup_file.sync_all().map_err(|error| error.to_string())?;
        drop(backup_file);
        backup_path = Some(candidate_backup.clone());
        replacement_attempted = true;
        replace_with_backup(&stage_path, &args.target, &candidate_backup)?;
        Ok(())
    })();
    if let Err(error) = result {
        let _ = std::fs::remove_file(&stage_path);
        if let Some(backup) = backup_path {
            if replacement_attempted {
                if crate::config::store::replace_file(&backup, &args.target).is_err() {
                    crate::update::log(&format!("回滚失败，备份保留在：{}", backup.display()));
                }
            } else {
                let _ = std::fs::remove_file(backup);
            }
        }
        return fail_and_restart(args, &format!("自更新失败：{error}"));
    }
    remove_zone_identifier(&args.target);
    crate::update::log(&format!("自更新完成：{}", args.target.display()));
    if args.restart {
        if let Err(error) = Command::new(&args.target).spawn() {
            if let Some(backup) = backup_path {
                if crate::config::store::replace_file(&backup, &args.target).is_err() {
                    crate::update::log(&format!("回滚失败，备份保留在：{}", backup.display()));
                }
            }
            let _ = std::fs::remove_file(&stage_path);
            crate::update::log(&format!("新版本启动失败：{error}"));
            return fail_and_restart(args, "自更新已替换，但新版本启动失败。");
        }
        crate::update::log("已重新启动新版本。");
    }
    0
}

fn hash_reader(reader: &mut File) -> std::io::Result<(u64, String)> {
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    let mut total = 0;
    loop {
        let read = reader.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        total += read as u64;
        hasher.update(&buffer[..read]);
    }
    Ok((total, to_hex(&hasher.finalize())))
}

fn copy_and_hash(source: &mut File, target: &mut File) -> std::io::Result<(u64, String)> {
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    let mut total = 0;
    loop {
        let read = source.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        target.write_all(&buffer[..read])?;
        hasher.update(&buffer[..read]);
        total += read as u64;
    }
    target.flush()?;
    Ok((total, to_hex(&hasher.finalize())))
}

fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02X}")).collect()
}

fn replace_with_backup(stage: &Path, target: &Path, backup: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    fn wide(path: &Path) -> Vec<u16> {
        path.as_os_str().encode_wide().chain(Some(0)).collect()
    }
    let ok = unsafe {
        windows_sys::Win32::Storage::FileSystem::ReplaceFileW(
            wide(stage).as_ptr(),
            wide(target).as_ptr(),
            wide(backup).as_ptr(),
            0,
            std::ptr::null(),
            std::ptr::null(),
        )
    };
    if ok == 0 {
        Err(std::io::Error::last_os_error().to_string())
    } else {
        Ok(())
    }
}

/// 复制当前 exe 为唯一助手并启动；握手成功后才允许调用方退出。
pub fn spawn_helper(target: &Path, new_file: &Path) -> Result<(), String> {
    let current =
        std::env::current_exe().map_err(|error| format!("读取当前程序路径失败：{error}"))?;
    let directory = ensure_update_directory()?;
    let helper = directory.join(format!(
        "NetConditioner-updater-{}.exe",
        crate::config::new_id()
    ));
    let mut source = File::open(&current).map_err(|error| format!("打开更新程序失败：{error}"))?;
    source
        .seek(SeekFrom::Start(0))
        .map_err(|error| error.to_string())?;
    let mut helper_file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&helper)
        .map_err(|error| format!("准备更新程序失败：{error}"))?;
    if let Err(error) =
        std::io::copy(&mut source, &mut helper_file).and_then(|_| helper_file.sync_all())
    {
        drop(helper_file);
        let _ = std::fs::remove_file(&helper);
        return Err(format!("准备更新程序失败：{error}"));
    }
    drop(helper_file);
    let (source_size, expected) =
        hash_file(new_file).map_err(|error| format!("校验更新包失败：{error}"))?;
    if source_size == 0 {
        let _ = std::fs::remove_file(&helper);
        return Err("更新包为空。".to_string());
    }
    let ready_name = format!(
        "Local\\NetConditioner-update-ready-{}",
        crate::config::new_id()
    );
    let ready_wide: Vec<u16> = ready_name.encode_utf16().chain(Some(0)).collect();
    let mut child: Option<Child> = None;
    let result = (|| -> Result<(), String> {
        let event = unsafe { CreateEventW(std::ptr::null(), 1, 0, ready_wide.as_ptr()) };
        if event.is_null() {
            return Err(std::io::Error::last_os_error().to_string());
        }
        let spawned = Command::new(&helper)
            .arg(APPLY_FLAG)
            .arg("--target")
            .arg(target)
            .arg("--new")
            .arg(new_file)
            .arg("--pid")
            .arg(std::process::id().to_string())
            .arg("--sha256")
            .arg(&expected)
            .arg("--size")
            .arg(source_size.to_string())
            .arg("--ready")
            .arg(&ready_name)
            .creation_flags(CREATE_NO_WINDOW)
            .spawn();
        let spawned = match spawned {
            Ok(child) => child,
            Err(error) => {
                unsafe {
                    CloseHandle(event);
                }
                return Err(error.to_string());
            }
        };
        child = Some(spawned);
        let wait = unsafe { WaitForSingleObject(event, 10_000) };
        unsafe {
            CloseHandle(event);
        }
        if wait != WAIT_OBJECT_0 {
            return Err("更新程序握手超时或失败。".to_string());
        }
        Ok(())
    })();
    if result.is_err() {
        if let Some(mut child) = child {
            let _ = child.kill();
            let _ = child.wait();
        }
        let _ = std::fs::remove_file(&helper);
    }
    result.map_err(|error| format!("启动更新程序失败：{error}"))
}

/// 去掉下载文件上的 Zone.Identifier 备用数据流，避免静默启动新版本时被 SmartScreen 拦截。
pub fn remove_zone_identifier(path: &Path) {
    let mut stream = path.as_os_str().to_os_string();
    stream.push(":Zone.Identifier");
    let wide: Vec<u16> = stream.encode_wide().chain(Some(0)).collect();
    unsafe {
        windows_sys::Win32::Storage::FileSystem::DeleteFileW(wide.as_ptr());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn helper_requires_explicit_positive_size_and_hash() {
        let sha = "A".repeat(64);
        let args = vec![
            "--apply-update",
            "--target",
            "a",
            "--new",
            "b",
            "--pid",
            "0",
            "--sha256",
            sha.as_str(),
            "--size",
            "0",
            "--ready",
            "e",
        ];
        assert!(parse_args(&args.iter().map(|s| s.to_string()).collect::<Vec<_>>()).is_none());
    }

    #[test]
    fn wait_distinguishes_timeout_failure_and_invalid_process() {
        assert_eq!(
            wait_for_process(std::process::id(), Duration::ZERO),
            ProcessWait::TimedOut
        );
        assert_eq!(
            wait_for_process(0, Duration::ZERO),
            ProcessWait::Failed(ERROR_ACCESS_DENIED)
        );
    }

    #[test]
    fn wait_status_constants_are_distinct() {
        assert_ne!(WAIT_OBJECT_0, WAIT_TIMEOUT);
        assert_ne!(WAIT_OBJECT_0, u32::MAX);
    }
}
