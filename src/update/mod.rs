//! 云更新的网络与自更新部分。

pub mod coordinator;

use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Duration;

use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::neturl;
use crate::version::{self, Version};

pub const MAX_MANIFEST_BYTES: usize = 64 * 1024;
pub const MAX_INSTALLER_BYTES: i64 = 300 * 1024 * 1024;
const CHECK_TIMEOUT: Duration = Duration::from_secs(10);
const DOWNLOAD_TIMEOUT: Duration = Duration::from_secs(300);

#[derive(Debug, Clone, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct UpdateInstallerInfo {
    pub url: Option<String>,
    pub sha256: Option<String>,
    #[serde(deserialize_with = "required_size")]
    pub size: i64,
}

fn required_size<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<i64, D::Error> {
    i64::deserialize(deserializer)
}

fn required_schema_version<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<i32, D::Error> {
    i32::deserialize(deserializer)
}

#[derive(Debug, Clone, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct UpdateManifest {
    #[serde(deserialize_with = "required_schema_version")]
    pub schema_version: i32,
    pub version: Option<String>,
    pub published_at: Option<String>,
    pub notes: Option<String>,
    pub page_url: Option<String>,
    pub installer: Option<UpdateInstallerInfo>,
}

#[derive(Debug, Clone)]
pub enum CheckStatus {
    UpToDate,
    UpdateAvailable(UpdateManifest, Version),
    NotConfigured(String),
    Failed(String),
}

pub fn update_directory() -> PathBuf {
    let base =
        crate::config::store::absolute_env_dir("LOCALAPPDATA").unwrap_or_else(known_local_appdata);
    base.join("NetConditioner").join("update")
}

fn known_local_appdata() -> PathBuf {
    use std::os::windows::ffi::OsStringExt;
    use windows_sys::Win32::UI::Shell::{
        FOLDERID_LocalAppData, SHGetKnownFolderPath, KF_FLAG_DEFAULT,
    };
    let mut path = std::ptr::null_mut();
    let result = unsafe {
        SHGetKnownFolderPath(
            &FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT as u32,
            std::ptr::null_mut(),
            &mut path,
        )
    };
    assert!(
        result >= 0 && !path.is_null(),
        "LocalAppData is unavailable"
    );
    let value =
        unsafe { std::ffi::OsString::from_wide(std::slice::from_raw_parts(path, wide_len(path))) };
    unsafe { crate::config::store::co_task_mem_free(path.cast()) };
    PathBuf::from(value)
}

unsafe fn wide_len(mut value: *const u16) -> usize {
    let start = value;
    while *value != 0 {
        value = value.add(1);
    }
    value.offset_from(start) as usize
}

pub fn ensure_update_directory() -> Result<PathBuf, String> {
    let directory = update_directory();
    fs::create_dir_all(&directory).map_err(|error| format!("创建更新目录失败：{error}"))?;
    // Best effort on filesystems without Windows ACLs; creation remains functional.
    let _ = restrict_directory_to_user(&directory);
    Ok(directory)
}

fn restrict_directory_to_user(directory: &Path) -> std::io::Result<()> {
    use windows_sys::Win32::Security::Authorization::ConvertStringSecurityDescriptorToSecurityDescriptorW;
    use windows_sys::Win32::Security::{
        SetFileSecurityW, DACL_SECURITY_INFORMATION, PROTECTED_DACL_SECURITY_INFORMATION,
    };
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn LocalFree(value: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    }
    let sddl: Vec<u16> = "D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)\0"
        .encode_utf16()
        .collect();
    let path: Vec<u16> = directory.as_os_str().encode_wide().chain(Some(0)).collect();
    unsafe {
        let mut descriptor = std::ptr::null_mut();
        if ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.as_ptr(),
            1,
            &mut descriptor,
            std::ptr::null_mut(),
        ) == 0
        {
            return Err(std::io::Error::last_os_error());
        }
        let result = SetFileSecurityW(
            path.as_ptr(),
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            descriptor,
        );
        LocalFree(descriptor.cast());
        if result == 0 {
            Err(std::io::Error::last_os_error())
        } else {
            Ok(())
        }
    }
}

pub fn log_path() -> PathBuf {
    update_directory().join("update.log")
}

pub fn payload_path(version: &str) -> PathBuf {
    let safe = version::parse(version)
        .map(|value| value.format())
        .unwrap_or_else(|| "unknown".to_string());
    update_directory().join(format!("NetConditioner-{safe}-win-x64.exe"))
}

pub fn is_dry_run() -> bool {
    matches!(std::env::var("NC_UPDATE_DRY_RUN"), Ok(value) if value == "1")
}

pub fn compute_sha256_hex(data: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(data);
    to_hex(&hasher.finalize())
}

fn to_hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        text.push_str(&format!("{byte:02X}"));
    }
    text
}

/// 解析并校验 latest.json；version 为规范化后的 4 段版本。
pub fn try_parse_manifest(json: &str) -> Result<(UpdateManifest, Version), String> {
    let mut manifest: UpdateManifest =
        serde_json::from_str(json).map_err(|_| "更新信息不是有效的 JSON。".to_string())?;

    if manifest.schema_version != 2 {
        return Err(format!(
            "不支持的更新信息版本：{}。",
            manifest.schema_version
        ));
    }

    let version = manifest
        .version
        .as_deref()
        .and_then(version::parse)
        .ok_or_else(|| "更新信息中的版本号无效。".to_string())?;

    let installer = manifest
        .installer
        .as_mut()
        .ok_or_else(|| "更新信息缺少更新包信息。".to_string())?;

    let url = installer
        .url
        .as_deref()
        .ok_or_else(|| "更新包地址无效。".to_string())?;
    let parsed_url = neturl::validate_installer_url(url)?;

    let sha256 = installer
        .sha256
        .as_deref()
        .and_then(neturl::try_normalize_sha256)
        .ok_or_else(|| "更新信息中的 SHA256 无效。".to_string())?;

    if installer.size <= 0 || installer.size > MAX_INSTALLER_BYTES {
        return Err("更新包大小超出允许范围。".to_string());
    }

    if let Some(notes) = &manifest.notes {
        if notes.chars().count() > 4000 {
            return Err("更新说明过长。".to_string());
        }
    }

    if let Some(page_url) = &manifest.page_url {
        if !page_url.trim().is_empty() && url::Url::parse(page_url).is_err() {
            return Err("更新页面地址无效。".to_string());
        }
    }

    installer.url = Some(parsed_url.to_string());
    installer.sha256 = Some(sha256);
    Ok((manifest, version))
}

fn build_agent(timeout: Duration, https_only: bool) -> ureq::Agent {
    // 使用 native-tls 和平台根证书；ureq 需要显式选择 TLS 后端。
    let tls = ureq::tls::TlsConfig::builder()
        .provider(ureq::tls::TlsProvider::NativeTls)
        .root_certs(ureq::tls::RootCerts::PlatformVerifier)
        .build();
    ureq::Agent::config_builder()
        .timeout_global(Some(timeout))
        .http_status_as_error(false)
        .https_only(https_only)
        .max_redirects(0)
        .user_agent("NetConditioner-Updater/1.0")
        .tls_config(tls)
        .build()
        .into()
}

/// Redirects are handled explicitly so every Location is checked before it is
/// requested. https_only remains enabled for all production requests.
macro_rules! get_checked {
    ($agent:expr, $url:expr, $loopback:expr) => {{
        (|| -> Result<_, String> {
            let mut next = url::Url::parse($url).map_err(|_| "更新地址无效。".to_string())?;
            for _ in 0..11 {
                neturl::validate_public_target(&next, $loopback)?;
                let response = $agent.get(next.as_str()).call().map_err(|error| {
                    if matches!(error, ureq::Error::Timeout(_)) {
                        "请求超时。".to_string()
                    } else {
                        format!("网络请求失败：{error}")
                    }
                })?;
                if !matches!(response.status().as_u16(), 301 | 302 | 303 | 307 | 308) {
                    return Ok(response);
                }
                let location = response
                    .headers()
                    .get("location")
                    .and_then(|value| value.to_str().ok())
                    .ok_or_else(|| "重定向缺少地址。".to_string())?;
                let resolved = next
                    .join(location)
                    .map_err(|_| "重定向地址无效。".to_string())?;
                neturl::validate_redirect(&next, &resolved)?;
                next = resolved;
            }
            Err("重定向次数过多。".to_string())
        })()
    }};
}

pub fn check(feed_url: &str, current: Version) -> CheckStatus {
    if let Err(error) = neturl::try_validate_feed_url(feed_url) {
        return CheckStatus::NotConfigured(error);
    }

    let loopback = url::Url::parse(feed_url)
        .map(|url| url.scheme() == "http")
        .unwrap_or(false);
    let agent = build_agent(CHECK_TIMEOUT, !loopback);
    let response = match get_checked!(agent, feed_url, loopback) {
        Ok(response) => response,
        Err(error) => return CheckStatus::Failed(format!("检查更新失败：{error}")),
    };

    let status = response.status().as_u16();
    if !(200..300).contains(&status) {
        return CheckStatus::Failed(format!("更新服务器返回 HTTP {status}。"));
    }

    let mut response = response;
    let bytes = match response
        .body_mut()
        .with_config()
        .limit(MAX_MANIFEST_BYTES as u64)
        .read_to_vec()
    {
        Ok(bytes) => bytes,
        Err(_) => return CheckStatus::Failed("更新信息过大，已忽略。".to_string()),
    };

    let json = String::from_utf8_lossy(&bytes);
    match try_parse_manifest(&json) {
        Ok((manifest, version)) => {
            if version::is_newer(current, version) {
                CheckStatus::UpdateAvailable(manifest, version)
            } else {
                CheckStatus::UpToDate
            }
        }
        Err(error) => CheckStatus::Failed(error),
    }
}

static DOWNLOAD_LOCK: Mutex<()> = Mutex::new(());

/// 下载更新包到唯一 staging 文件，校验大小与哈希后原子替换目标。
pub fn download(manifest: &UpdateManifest, destination: &Path) -> bool {
    let _guard = DOWNLOAD_LOCK
        .lock()
        .unwrap_or_else(|error| error.into_inner());
    match download_inner(manifest, destination) {
        Ok(total) => {
            crate::selfupdate::remove_zone_identifier(destination);
            log(&format!(
                "下载完成并校验通过：{}（{total} 字节）",
                destination.display()
            ));
            true
        }
        Err(error) => {
            log(&format!("下载校验失败：{error}"));
            false
        }
    }
}

fn download_inner(manifest: &UpdateManifest, destination: &Path) -> Result<u64, String> {
    let installer = manifest.installer.as_ref().ok_or("更新包信息无效。")?;
    let url = installer.url.as_deref().ok_or("更新包地址无效。")?;
    let expected = installer
        .sha256
        .as_deref()
        .and_then(neturl::try_normalize_sha256)
        .ok_or("SHA256 无效。")?;
    if installer.size <= 0 || installer.size > MAX_INSTALLER_BYTES {
        return Err("更新包大小超出允许范围。".to_string());
    }
    if let Some(parent) = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
    {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    let (part, mut file) = create_staging_file(destination, "nc-download")?;
    let result = (|| {
        let loopback = url::Url::parse(url)
            .map(|url| url.scheme() == "http")
            .unwrap_or(false);
        let agent = build_agent(DOWNLOAD_TIMEOUT, !loopback);
        let mut response = get_checked!(agent, url, loopback)?;
        let status = response.status().as_u16();
        if !(200..300).contains(&status) {
            return Err(format!("服务器返回 HTTP {status}。"));
        }
        let mut total = 0u64;
        let mut hasher = Sha256::new();
        let mut reader = response.body_mut().as_reader();
        let mut buffer = [0u8; 64 * 1024];
        loop {
            let read = reader
                .read(&mut buffer)
                .map_err(|error| format!("读取更新包失败：{error}"))?;
            if read == 0 {
                break;
            }
            total += read as u64;
            if total > MAX_INSTALLER_BYTES as u64 {
                return Err("更新包超过允许的最大体积。".to_string());
            }
            hasher.update(&buffer[..read]);
            file.write_all(&buffer[..read])
                .map_err(|error| format!("写入更新包失败：{error}"))?;
        }
        if total == 0 {
            return Err("更新包为空。".to_string());
        }
        if total != installer.size as u64 {
            return Err(format!(
                "文件大小 {total} 与清单 {} 不一致。",
                installer.size
            ));
        }
        if !to_hex(&hasher.finalize()).eq_ignore_ascii_case(&expected) {
            return Err("SHA256 不匹配。".to_string());
        }
        file.sync_all()
            .map_err(|error| format!("同步更新包失败：{error}"))?;
        drop(file);
        crate::config::store::replace_file(&part, destination)
            .map_err(|error| error.to_string())?;
        let (final_size, final_hash) = hash_file(destination)?;
        if final_size != installer.size as u64 || !final_hash.eq_ignore_ascii_case(&expected) {
            return Err("最终更新包复核失败。".to_string());
        }
        Ok(final_size)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&part);
    }
    result
}

pub(crate) fn create_staging_file(
    destination: &Path,
    tag: &str,
) -> Result<(PathBuf, File), String> {
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    for _ in 0..32 {
        let path = parent.join(format!("{tag}-{}.tmp", crate::config::new_id()));
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);
        match options.open(&path) {
            Ok(file) => return Ok((path, file)),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error.to_string()),
        }
    }
    Err("无法创建唯一更新临时文件。".to_string())
}

pub(crate) fn hash_file(path: &Path) -> Result<(u64, String), String> {
    let mut file = File::open(path).map_err(|error| error.to_string())?;
    let mut hasher = Sha256::new();
    let mut total = 0u64;
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let read = file.read(&mut buffer).map_err(|error| error.to_string())?;
        if read == 0 {
            break;
        }
        total += read as u64;
        hasher.update(&buffer[..read]);
    }
    Ok((total, to_hex(&hasher.finalize())))
}

/// 启动自更新助手：等待当前进程退出后通过文件复制覆盖自身并重启。
pub fn apply_self_update(new_file: &Path) -> bool {
    if is_dry_run() {
        log("DRY RUN：跳过实际更新。");
        log(&format!("DRY RUN 更新包：{}", new_file.display()));
        match std::env::current_exe() {
            Ok(target) => log(&format!("DRY RUN 目标：{}", target.display())),
            Err(error) => log(&format!("DRY RUN 目标：读取当前程序路径失败（{error}）")),
        }
        return true;
    }

    if !new_file.exists() {
        log(&format!(
            "启动自更新失败：更新包不存在（{}）。",
            new_file.display()
        ));
        return false;
    }

    let target = match std::env::current_exe() {
        Ok(path) => path,
        Err(error) => {
            log(&format!(
                "启动自更新失败：读取当前程序路径失败（{error}）。"
            ));
            return false;
        }
    };

    match crate::selfupdate::spawn_helper(&target, new_file) {
        Ok(()) => {
            log(&format!("已启动自更新：{}", new_file.display()));
            true
        }
        Err(error) => {
            log(&format!("启动自更新失败：{error}"));
            false
        }
    }
}

/// 清理更新目录中的旧文件（默认 7 天），应用启动时调用。
pub fn cleanup_old_files() {
    let directory = update_directory();
    let Ok(entries) = fs::read_dir(&directory) else {
        return;
    };
    let cutoff = std::time::SystemTime::now() - Duration::from_secs(7 * 24 * 60 * 60);
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(metadata) = entry.metadata() else {
            continue;
        };
        if !metadata.is_file() {
            continue;
        }
        if metadata
            .modified()
            .map(|time| time < cutoff)
            .unwrap_or(false)
        {
            let _ = fs::remove_file(path);
        }
    }
}

pub fn log(message: &str) {
    ensure_update_directory().ok();
    let path = log_path();

    if let Ok(metadata) = fs::metadata(&path) {
        if metadata.len() > 512 * 1024 {
            let mut backup = path.as_os_str().to_os_string();
            backup.push(".old");
            let _ = fs::rename(&path, PathBuf::from(backup));
        }
    }

    let line = format!(
        "[{}] {message}\r\n",
        chrono::Local::now().format("%Y-%m-%d %H:%M:%S%.3f")
    );
    if let Ok(mut file) = fs::OpenOptions::new().create(true).append(true).open(&path) {
        let _ = file.write_all(line.as_bytes());
    }
}

/// 更新服务门面。
pub struct UpdateService;

impl UpdateService {
    pub fn payload_path(version: &str) -> PathBuf {
        payload_path(version)
    }

    pub fn is_dry_run() -> bool {
        is_dry_run()
    }

    pub fn compute_sha256_hex(data: &[u8]) -> String {
        compute_sha256_hex(data)
    }

    pub fn try_parse_manifest(json: &str) -> Result<(UpdateManifest, Version), String> {
        try_parse_manifest(json)
    }

    pub fn check(feed_url: &str, current: Version) -> CheckStatus {
        check(feed_url, current)
    }

    pub fn download(manifest: &UpdateManifest, destination: &Path) -> bool {
        download(manifest, destination)
    }

    pub fn apply_self_update(new_file: &Path) -> bool {
        apply_self_update(new_file)
    }

    pub fn cleanup_old_files() {
        cleanup_old_files()
    }

    pub fn log(message: &str) {
        log(message)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_manifest() -> String {
        let sha = "a".repeat(64);
        format!(
            "{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"notes\":\"test\",\"installer\":{{\"url\":\"https://example.com/NetConditioner-9.9.9-win-x64.exe\",\"sha256\":\"{sha}\",\"size\":123}}}}"
        )
    }

    #[test]
    fn accepts_valid_manifest() {
        let (manifest, version) = try_parse_manifest(&sample_manifest()).expect("valid");
        assert_eq!(version.format(), "9.9.9");
        let installer = manifest.installer.as_ref().expect("installer");
        assert_eq!(installer.sha256.as_deref(), Some("A".repeat(64).as_str()));
        assert!(installer.url.as_deref().unwrap().starts_with("https://"));
    }

    #[test]
    fn rejects_invalid_manifests() {
        let sha = "a".repeat(64);
        let invalid = [
            format!("{{\"schemaVersion\":2,\"installer\":{{\"url\":\"https://example.com/a.exe\",\"sha256\":\"{sha}\"}}}}"),
            "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://example.com/a.exe\",\"sha256\":\"abc\"}}".to_string(),
            format!("{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{{\"url\":\"http://example.com/a.exe\",\"sha256\":\"{sha}\"}}}}"),
            "{\"schemaVersion\":1,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://example.com/a.exe\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}".to_string(),
            "not json".to_string(),
        ];
        for manifest in invalid {
            assert!(try_parse_manifest(&manifest).is_err());
        }
    }

    #[test]
    fn rejects_out_of_range_fields() {
        let sha = "a".repeat(64);
        let negative_size = format!(
            "{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{{\"url\":\"https://example.com/a.exe\",\"sha256\":\"{sha}\",\"size\":-1}}}}"
        );
        assert!(try_parse_manifest(&negative_size).is_err());

        let huge_size = format!(
            "{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{{\"url\":\"https://example.com/a.exe\",\"sha256\":\"{sha}\",\"size\":{}}}}}",
            MAX_INSTALLER_BYTES + 1
        );
        assert!(try_parse_manifest(&huge_size).is_err());

        let long_notes = format!(
            "{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"notes\":\"{}\",\"installer\":{{\"url\":\"https://example.com/a.exe\",\"sha256\":\"{sha}\"}}}}",
            "x".repeat(4001)
        );
        assert!(try_parse_manifest(&long_notes).is_err());
    }

    #[test]
    fn manifest_requires_explicit_positive_size_and_schema() {
        let sha = "a".repeat(64);
        for json in [
            format!(
                r#"{{"version":"9.9.9","installer":{{"url":"https://example.com/a.exe","sha256":"{sha}","size":1}}}}"#
            ),
            format!(
                r#"{{"schemaVersion":2,"version":"9.9.9","installer":{{"url":"https://example.com/a.exe","sha256":"{sha}"}}}}"#
            ),
            format!(
                r#"{{"schemaVersion":2,"version":"9.9.9","installer":{{"url":"https://example.com/a.exe","sha256":"{sha}","size":0}}}}"#
            ),
            format!(
                r#"{{"schemaVersion":2,"version":"9.9.9","installer":{{"url":"https://example.com/a.exe","sha256":"{sha}","size":null}}}}"#
            ),
        ] {
            assert!(try_parse_manifest(&json).is_err(), "accepted {json}");
        }
    }

    fn serve_test(
        body: Vec<u8>,
        count: usize,
        location: Option<String>,
    ) -> (String, std::thread::JoinHandle<()>) {
        use std::io::Write;
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://{}", listener.local_addr().unwrap());
        let handle = std::thread::spawn(move || {
            for _ in 0..count {
                let (mut stream, _) = listener.accept().unwrap();
                let mut request = [0u8; 4096];
                let _ = stream.read(&mut request);
                let headers = match &location {
                    Some(location) => format!("HTTP/1.1 302 Found\r\nLocation: {location}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"),
                    None => format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()),
                };
                let _ = stream.write_all(headers.as_bytes());
                if location.is_none() {
                    let _ = stream.write_all(&body);
                }
            }
        });
        (base, handle)
    }

    #[test]
    fn redirects_are_followed_and_unsafe_redirects_rejected() {
        // A separate server supplies the final response; the first response
        // redirects there and is never read as a manifest.
        let (final_base, final_server) = serve_test(sample_manifest().into_bytes(), 1, None);
        let (initial, first) = serve_test(Vec::new(), 1, Some(format!("{final_base}/latest.json")));
        match check(&initial, version::current()) {
            CheckStatus::UpdateAvailable(..) => {}
            other => panic!("valid redirect failed: {other:?}"),
        }
        first.join().unwrap();
        final_server.join().unwrap();
        let (bad, bad_server) = serve_test(Vec::new(), 1, Some("http://192.168.1.1/a".to_string()));
        assert!(matches!(
            check(&bad, version::current()),
            CheckStatus::Failed(_)
        ));
        bad_server.join().unwrap();
    }

    #[test]
    fn tampered_and_empty_downloads_never_succeed() {
        let body = b"tampered payload".to_vec();
        let (base, server) = serve_test(body.clone(), 1, None);
        let manifest = UpdateManifest {
            installer: Some(UpdateInstallerInfo {
                url: Some(format!("{base}/a.exe")),
                sha256: Some("A".repeat(64)),
                size: body.len() as i64,
            }),
            ..Default::default()
        };
        let destination =
            std::env::temp_dir().join(format!("nc-tamper-{}.exe", crate::config::new_id()));
        assert!(!download(&manifest, &destination));
        assert!(!destination.exists());
        server.join().unwrap();
    }

    #[test]
    fn empty_payload_is_rejected_even_with_matching_hash() {
        let (base, server) = serve_test(Vec::new(), 1, None);
        let manifest = UpdateManifest {
            installer: Some(UpdateInstallerInfo {
                url: Some(format!("{base}/a.exe")),
                sha256: Some(compute_sha256_hex(b"")),
                size: 1,
            }),
            ..UpdateManifest::default()
        };
        let destination =
            std::env::temp_dir().join(format!("nc-empty-{}.exe", crate::config::new_id()));
        assert!(!download(&manifest, &destination));
        assert!(!destination.exists());
        server.join().unwrap();
    }

    #[test]
    fn concurrent_downloads_use_unique_staging_and_valid_final_payload() {
        let body = b"verified payload".to_vec();
        let (base, server) = serve_test(body.clone(), 8, None);
        let destination =
            std::env::temp_dir().join(format!("nc-concurrent-{}.exe", crate::config::new_id()));
        let threads: Vec<_> = (0..8)
            .map(|_| {
                let base = base.clone();
                let destination = destination.clone();
                let body = body.clone();
                std::thread::spawn(move || {
                    let manifest = UpdateManifest {
                        installer: Some(UpdateInstallerInfo {
                            url: Some(format!("{base}/a.exe")),
                            sha256: Some(compute_sha256_hex(&body)),
                            size: body.len() as i64,
                        }),
                        ..UpdateManifest::default()
                    };
                    assert!(download(&manifest, &destination));
                })
            })
            .collect();
        for thread in threads {
            thread.join().unwrap();
        }
        server.join().unwrap();
        assert_eq!(fs::read(&destination).unwrap(), body);
        let _ = fs::remove_file(destination);
    }

    #[test]
    fn sha256_known_vector() {
        assert_eq!(
            compute_sha256_hex(b"abc"),
            "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"
        );
    }

    /// 真实网络联调：检查 GitHub Releases 上的清单并下载更新包。
    /// 默认忽略，需要时执行 `cargo test --release -- --ignored`。
    #[test]
    #[ignore]
    fn github_release_manifest_and_download() {
        const FEED: &str =
            "https://github.com/Tpwalk1145/NetConditioner/releases/latest/download/latest.json";

        match check(FEED, version::current()) {
            CheckStatus::UpdateAvailable(..) | CheckStatus::UpToDate => {}
            CheckStatus::NotConfigured(error) | CheckStatus::Failed(error) => {
                panic!("无法访问 GitHub 更新清单：{error}")
            }
        }

        let agent = build_agent(CHECK_TIMEOUT, true);
        let mut response = agent.get(FEED).call().expect("GET latest.json");
        assert!((200..300).contains(&response.status().as_u16()));
        let body = response
            .body_mut()
            .with_config()
            .limit(MAX_MANIFEST_BYTES as u64)
            .read_to_vec()
            .expect("读取 latest.json");
        let (manifest, release_version) =
            try_parse_manifest(&String::from_utf8_lossy(&body)).expect("解析 latest.json");
        println!("GitHub 上的最新版本：{}", release_version.format());

        let destination = std::env::temp_dir().join("NetConditioner-github-download-test.exe");
        let _ = fs::remove_file(&destination);
        assert!(download(&manifest, &destination), "下载更新包失败");

        let bytes = fs::read(&destination).expect("读取下载的更新包");
        let actual = compute_sha256_hex(&bytes);
        let installer = manifest.installer.as_ref().expect("installer");
        assert_eq!(actual, installer.sha256.as_deref().unwrap_or_default());
        if installer.size > 0 {
            assert_eq!(bytes.len() as i64, installer.size);
        }

        let _ = fs::remove_file(&destination);
    }
}
