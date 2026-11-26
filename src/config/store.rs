//! 配置文件的读取、校验与原子写入。

use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::windows::ffi::OsStringExt;
use std::path::{Path, PathBuf};

use crate::config::{self, AppConfig};

pub const MAX_CONFIG_BYTES: u64 = 1024 * 1024;

/// 读取结果：总是返回可用的配置，失败细节放在 warning 里。
pub struct LoadResult {
    pub config: AppConfig,
    pub warning: Option<String>,
}

pub struct ProfileStore {
    path: PathBuf,
}

impl ProfileStore {
    pub fn new(path: PathBuf) -> Self {
        Self { path }
    }

    pub fn with_default_path() -> Self {
        Self::new(Self::default_config_path())
    }

    pub fn default_config_path() -> PathBuf {
        let base = absolute_env_dir("APPDATA").unwrap_or_else(known_appdata);
        base.join("NetConditioner").join("config.json")
    }

    pub fn load(&self) -> LoadResult {
        let _lock = StoreLock::acquire(&self.path);
        match read_config_bounded(&self.path) {
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                let mut fresh = AppConfig::fresh();
                config::sanitize(&mut fresh, config::now_unix());
                let warning = self
                    .save_locked(&fresh)
                    .err()
                    .map(|error| format!("保存配置失败：{error}"));
                LoadResult {
                    config: fresh,
                    warning,
                }
            }
            Err(error) => {
                if fs::metadata(&self.path).is_ok_and(|metadata| metadata.len() > MAX_CONFIG_BYTES)
                {
                    return LoadResult {
                        config: AppConfig::fresh(),
                        warning: Some("配置文件超过 1 MiB 限制，未修改原文件。".to_string()),
                    };
                }
                self.recover(error.to_string())
            }
            Ok(bytes) => {
                let Ok(text) = String::from_utf8(bytes) else {
                    return self.recover("配置文件不是 UTF-8。".to_string());
                };
                let Ok(parsed) = serde_json::from_str::<serde_json::Value>(&text) else {
                    return self.recover("配置文件不是有效的 JSON。".to_string());
                };
                if parsed.get("version").and_then(|value| value.as_i64()) != Some(1)
                    || parsed
                        .get("schemaVersion")
                        .is_some_and(|value| value.as_i64() != Some(1))
                {
                    return LoadResult {
                        config: AppConfig::fresh(),
                        warning: Some(
                            "配置文件 schemaVersion 不受支持，未修改原文件。".to_string(),
                        ),
                    };
                }
                let Ok(mut config) = serde_json::from_value::<AppConfig>(parsed.clone()) else {
                    return self.recover("配置字段损坏。".to_string());
                };
                let outcome = config::sanitize(&mut config, config::now_unix());
                let mut warning = (outcome.removed_profiles > 0)
                    .then(|| format!("已忽略 {} 个损坏的配置项。", outcome.removed_profiles));
                // 仅在内容实际变化时写回；否则原样保留（包括未知字段）。
                let serialized = serde_json::to_value(&config).ok();
                if serialized
                    .as_ref()
                    .is_none_or(|value| !known_fields_equal(&parsed, value))
                {
                    if let Err(error) = self.save_locked(&config) {
                        let message = format!("保存配置失败：{error}");
                        warning = Some(match warning {
                            Some(existing) => format!("{existing}\r\n{message}"),
                            None => message,
                        });
                    }
                }
                LoadResult { config, warning }
            }
        }
    }

    pub fn save(&self, config: &AppConfig) -> Result<(), String> {
        let _lock = StoreLock::acquire(&self.path);
        self.save_locked(config)
    }

    fn save_locked(&self, config: &AppConfig) -> Result<(), String> {
        if let Some(parent) = self
            .path
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
        {
            fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }
        let existing = fs::metadata(&self.path)
            .map(|metadata| metadata.len() <= MAX_CONFIG_BYTES)
            .unwrap_or(false)
            .then(|| fs::read(&self.path).ok())
            .flatten();
        let mut document = existing
            .as_deref()
            .and_then(|bytes| serde_json::from_slice::<serde_json::Value>(bytes).ok())
            .unwrap_or_else(|| serde_json::to_value(config).unwrap());
        let updated = serde_json::to_value(config).map_err(|error| error.to_string())?;
        merge_known_fields(&mut document, updated);
        let json = serde_json::to_vec_pretty(&document).map_err(|error| error.to_string())?;
        if existing.as_deref() == Some(json.as_slice()) {
            return Ok(());
        }
        let parent = self
            .path
            .parent()
            .filter(|path| !path.as_os_str().is_empty());
        let (temp, file) = create_new_temp(self.path.parent(), "nc-config", &json)?;
        if let Err(error) = file.sync_all() {
            drop(file);
            let _ = fs::remove_file(&temp);
            return Err(error.to_string());
        }
        drop(file);
        if let Err(error) = replace_file(&temp, &self.path) {
            let _ = fs::remove_file(&temp);
            return Err(error.to_string());
        }
        if let Some(parent) = parent {
            let _ = File::open(parent).and_then(|directory| directory.sync_all());
        }
        Ok(())
    }

    fn recover(&self, error: String) -> LoadResult {
        let mut fresh = AppConfig::fresh();
        config::sanitize(&mut fresh, config::now_unix());
        let warning = match self.backup_broken_config() {
            Ok(_) => {
                let message = format!("配置文件读取失败，已恢复默认配置。\r\n{error}");
                match self.save_locked(&fresh) {
                    Ok(()) => message,
                    Err(save_error) => format!("{message}\r\n保存配置失败：{save_error}"),
                }
            }
            Err(backup_error) => format!(
                "配置文件读取失败，且备份失败；保留原文件未覆盖。\r\n{error}\r\n备份失败：{backup_error}"
            ),
        };
        LoadResult {
            config: fresh,
            warning: Some(warning),
        }
    }

    fn backup_broken_config(&self) -> Result<PathBuf, String> {
        let (path, mut backup) = create_new_temp(self.path.parent(), "nc-config-backup", &[])?;
        let result = (|| -> std::io::Result<()> {
            let mut original = File::open(&self.path)?;
            std::io::copy(&mut original, &mut backup)?;
            backup.sync_all()
        })();
        if let Err(error) = result {
            let _ = fs::remove_file(&path);
            return Err(error.to_string());
        }
        Ok(path)
    }
}

fn read_config_bounded(path: &Path) -> std::io::Result<Vec<u8>> {
    let file = File::open(path)?;
    if file.metadata()?.len() > MAX_CONFIG_BYTES {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "配置文件超过 1 MiB 限制。",
        ));
    }
    let mut bytes = Vec::new();
    file.take(MAX_CONFIG_BYTES + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_CONFIG_BYTES {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "配置文件超过 1 MiB 限制。",
        ));
    }
    Ok(bytes)
}

pub(crate) fn absolute_env_dir(name: &str) -> Option<PathBuf> {
    let value = std::env::var_os(name)?;
    if value.is_empty() {
        return None;
    }
    let path = PathBuf::from(value);
    path.is_absolute().then_some(path)
}

fn known_appdata() -> PathBuf {
    use windows_sys::Win32::UI::Shell::{
        FOLDERID_RoamingAppData, SHGetKnownFolderPath, KF_FLAG_DEFAULT,
    };
    let mut path = std::ptr::null_mut();
    let result = unsafe {
        SHGetKnownFolderPath(
            &FOLDERID_RoamingAppData,
            KF_FLAG_DEFAULT as u32,
            std::ptr::null_mut(),
            &mut path,
        )
    };
    if result >= 0 && !path.is_null() {
        let value = unsafe {
            std::ffi::OsString::from_wide(std::slice::from_raw_parts(path, wcslen(path)))
        };
        unsafe { co_task_mem_free(path.cast()) };
        return PathBuf::from(value);
    }
    unreachable!("APPDATA and the roaming folder are unavailable")
}

#[link(name = "ole32")]
unsafe extern "system" {
    fn CoTaskMemFree(value: *mut core::ffi::c_void);
}
pub(crate) unsafe fn co_task_mem_free(value: *mut core::ffi::c_void) {
    CoTaskMemFree(value)
}

unsafe fn wcslen(mut value: *const u16) -> usize {
    let start = value;
    while *value != 0 {
        value = value.add(1);
    }
    value.offset_from(start) as usize
}

pub(crate) fn create_new_temp(
    parent: Option<&Path>,
    tag: &str,
    bytes: &[u8],
) -> Result<(PathBuf, File), String> {
    let parent = parent.unwrap_or_else(|| Path::new("."));
    for _ in 0..32 {
        let path = parent.join(format!("{tag}-{}.tmp", crate::config::new_id()));
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);
        match options.open(&path) {
            Ok(mut file) => {
                if let Err(error) = file.write_all(bytes) {
                    let _ = fs::remove_file(&path);
                    return Err(error.to_string());
                }
                if let Err(error) = file.flush() {
                    let _ = fs::remove_file(&path);
                    return Err(error.to_string());
                }
                return Ok((path, file));
            }
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error.to_string()),
        }
    }
    Err("无法创建唯一临时文件。".to_string())
}

fn known_fields_equal(original: &serde_json::Value, updated: &serde_json::Value) -> bool {
    use serde_json::Value;
    match (original, updated) {
        (Value::Object(old), Value::Object(new)) => new.iter().all(|(key, value)| {
            old.get(key)
                .is_some_and(|existing| known_fields_equal(existing, value))
        }),
        (Value::Array(old), Value::Array(new)) => {
            old.len() == new.len()
                && old
                    .iter()
                    .zip(new)
                    .all(|(old, new)| known_fields_equal(old, new))
        }
        (old, new) => old == new,
    }
}

fn merge_known_fields(original: &mut serde_json::Value, updated: serde_json::Value) {
    use serde_json::Value;
    match (original, updated) {
        (Value::Object(old), Value::Object(new)) => {
            for (key, value) in new {
                if let Some(existing) = old.get_mut(&key) {
                    merge_known_fields(existing, value);
                } else {
                    old.insert(key, value);
                }
            }
        }
        // Profile arrays are positional in the public model; preserve unknown
        // fields in surviving profiles while honoring removals and additions.
        (Value::Array(old), Value::Array(new)) => {
            let new_len = new.len();
            for (index, value) in new.into_iter().enumerate() {
                if let Some(existing) = old.get_mut(index) {
                    merge_known_fields(existing, value);
                } else {
                    old.push(value);
                }
            }
            old.truncate(new_len);
        }
        (old, new) => *old = new,
    }
}

pub(crate) fn replace_file(source: &Path, target: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    const MOVEFILE_REPLACE_EXISTING: u32 = 1;
    const MOVEFILE_WRITE_THROUGH: u32 = 8;
    fn wide(path: &Path) -> Vec<u16> {
        path.as_os_str().encode_wide().chain(Some(0)).collect()
    }
    let ok = unsafe {
        windows_sys::Win32::Storage::FileSystem::MoveFileExW(
            wide(source).as_ptr(),
            wide(target).as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if ok == 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}

struct StoreLock(windows_sys::Win32::Foundation::HANDLE);

impl StoreLock {
    fn acquire(path: &Path) -> Self {
        use std::collections::hash_map::DefaultHasher;
        use std::hash::{Hash, Hasher};
        let mut hash = DefaultHasher::new();
        path.hash(&mut hash);
        let name: Vec<u16> = format!("Local\\NetConditioner-config-{:016x}", hash.finish())
            .encode_utf16()
            .chain(Some(0))
            .collect();
        unsafe {
            let handle = windows_sys::Win32::System::Threading::CreateMutexW(
                std::ptr::null(),
                0,
                name.as_ptr(),
            );
            assert!(!handle.is_null(), "CreateMutexW failed");
            let result = windows_sys::Win32::System::Threading::WaitForSingleObject(handle, 10_000);
            assert!(
                matches!(result, 0 | 0x80),
                "config lock wait failed: {result}"
            );
            Self(handle)
        }
    }
}

impl Drop for StoreLock {
    fn drop(&mut self) {
        unsafe {
            release_mutex(self.0);
            windows_sys::Win32::Foundation::CloseHandle(self.0);
        }
    }
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn ReleaseMutex(handle: windows_sys::Win32::Foundation::HANDLE) -> i32;
}
unsafe fn release_mutex(handle: windows_sys::Win32::Foundation::HANDLE) {
    ReleaseMutex(handle);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("nc-store-{tag}-{}", crate::config::new_id()));
        fs::create_dir_all(&dir).expect("temp dir");
        dir
    }

    #[test]
    fn broken_config_is_backed_up_and_recovered() {
        let dir = temp_dir("broken");
        let path = dir.join("config.json");
        fs::write(&path, "{ not json").expect("write");
        let loaded = ProfileStore::new(path.clone()).load();
        assert!(loaded.warning.unwrap().contains("已恢复"));
        assert!(fs::read_dir(&dir).unwrap().flatten().any(|entry| {
            entry
                .file_name()
                .to_string_lossy()
                .starts_with("nc-config-backup-")
        }));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn backup_failure_never_overwrites_original() {
        let dir = temp_dir("backup-failure");
        let path = dir.join("config.json");
        // Reading a directory fails both the config read and the backup read;
        // this deterministically injects backup failure without ACL privileges.
        fs::create_dir(&path).unwrap();
        fs::write(path.join("original"), b"keep me").unwrap();
        let loaded = ProfileStore::new(path.clone()).load();
        assert!(loaded.warning.unwrap().contains("保留原文件未覆盖"));
        assert!(path.is_dir());
        assert_eq!(fs::read(path.join("original")).unwrap(), b"keep me");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn round_trip_keeps_fields_and_concurrent_saves() {
        let dir = temp_dir("roundtrip");
        let path = dir.join("config.json");
        let store = ProfileStore::new(path.clone());
        let mut config = AppConfig::fresh();
        config.settings.max_queue_delay_ms = 500;
        store.save(&config).expect("save");
        let threads: Vec<_> = (0..8)
            .map(|index| {
                let path = path.clone();
                std::thread::spawn(move || {
                    let mut config = AppConfig::fresh();
                    config.settings.max_run_minutes = index;
                    ProfileStore::new(path)
                        .save(&config)
                        .expect("concurrent save");
                })
            })
            .collect();
        for thread in threads {
            thread.join().unwrap();
        }
        let loaded = ProfileStore::new(path).load();
        assert_eq!(loaded.config.settings.max_queue_delay_ms, 2000);
        assert_eq!(loaded.config.profiles.len(), 1);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn valid_load_does_not_rewrite_unknown_fields() {
        let dir = temp_dir("unchanged");
        let path = dir.join("config.json");
        let store = ProfileStore::new(path.clone());
        store.save(&AppConfig::fresh()).unwrap();
        let mut value: serde_json::Value =
            serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        value["future"] = serde_json::json!({"nested": true});
        value["settings"]["futureSetting"] = serde_json::json!(42);
        fs::write(&path, serde_json::to_vec_pretty(&value).unwrap()).unwrap();
        let before = fs::read(&path).unwrap();
        let loaded = store.load();
        assert!(loaded.warning.is_none());
        assert_eq!(fs::read(&path).unwrap(), before);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn saving_preserves_unknown_nested_fields() {
        let dir = temp_dir("unknown");
        let path = dir.join("config.json");
        fs::write(&path, r#"{"version":1,"future":1,"profiles":[{"id":"p","name":"kept","futureProfile":2}],"settings":{"futureSetting":3}}"#).unwrap();
        let loaded = ProfileStore::new(path.clone()).load();
        ProfileStore::new(path.clone())
            .save(&loaded.config)
            .unwrap();
        let saved: serde_json::Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        assert_eq!(saved["future"], 1);
        assert_eq!(saved["profiles"][0]["futureProfile"], 2);
        assert_eq!(saved["settings"]["futureSetting"], 3);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn oversized_and_unsupported_configs_are_not_rewritten() {
        let dir = temp_dir("reject");
        let path = dir.join("config.json");
        let text = r#"{"schemaVersion":99,"future":1}"#;
        fs::write(&path, text).unwrap();
        assert!(ProfileStore::new(path.clone()).load().warning.is_some());
        assert_eq!(fs::read_to_string(&path).unwrap(), text);
        let text = " ".repeat(MAX_CONFIG_BYTES as usize + 1);
        fs::write(&path, &text).unwrap();
        assert!(ProfileStore::new(path.clone()).load().warning.is_some());
        assert_eq!(fs::read_to_string(path).unwrap(), text);
        let _ = fs::remove_dir_all(&dir);
    }
}
