//! 云更新的网络与自更新部分（对应原 UpdateService，单文件版）。

pub mod coordinator;

use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
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
    pub size: i64,
}

fn default_schema_version() -> i32 {
    2
}

#[derive(Debug, Clone, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct UpdateManifest {
    #[serde(default = "default_schema_version")]
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
    let base = std::env::var_os("TEMP")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    base.join("NetConditioner").join("update")
}

pub fn log_path() -> PathBuf {
    update_directory().join("update.log")
}

pub fn payload_path(version: &str) -> PathBuf {
    update_directory().join(format!("NetConditioner-{version}-win-x64.exe"))
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

    if installer.size < 0 || installer.size > MAX_INSTALLER_BYTES {
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

fn build_agent(timeout: Duration) -> ureq::Agent {
    // 必须显式指定 native-tls + 系统证书存储：ureq 默认是 rustls/WebPki，
    // 在本项目未启用对应 feature 时 https 请求会直接 panic。
    let tls = ureq::tls::TlsConfig::builder()
        .provider(ureq::tls::TlsProvider::NativeTls)
        .root_certs(ureq::tls::RootCerts::PlatformVerifier)
        .build();
    ureq::Agent::config_builder()
        .timeout_global(Some(timeout))
        .http_status_as_error(false)
        .max_redirects(10)
        .user_agent("NetConditioner-Updater/1.0")
        .tls_config(tls)
        .build()
        .into()
}

pub fn check(feed_url: &str, current: Version) -> CheckStatus {
    if let Err(error) = neturl::try_validate_feed_url(feed_url) {
        return CheckStatus::NotConfigured(error);
    }

    let agent = build_agent(CHECK_TIMEOUT);
    let response = match agent.get(feed_url).call() {
        Ok(response) => response,
        Err(error) => {
            return if matches!(error, ureq::Error::Timeout(_)) {
                CheckStatus::Failed("检查更新超时。".to_string())
            } else {
                CheckStatus::Failed(format!("检查更新失败：{error}"))
            };
        }
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

fn part_path(destination: &Path) -> PathBuf {
    let mut text = destination.as_os_str().to_os_string();
    text.push(".part");
    PathBuf::from(text)
}

/// 下载更新包到 destination，边下边算 SHA256，校验 size 与哈希后原子落盘。
pub fn download(manifest: &UpdateManifest, destination: &Path) -> bool {
    let Some(installer) = manifest.installer.as_ref() else {
        log("下载失败：更新包地址或 SHA256 无效。");
        return false;
    };
    let Some(url) = installer.url.as_deref() else {
        log("下载失败：更新包地址或 SHA256 无效。");
        return false;
    };
    let Some(expected) = installer
        .sha256
        .as_deref()
        .and_then(neturl::try_normalize_sha256)
    else {
        log("下载失败：更新包地址或 SHA256 无效。");
        return false;
    };

    let part = part_path(destination);
    let _ = fs::create_dir_all(update_directory());

    let agent = build_agent(DOWNLOAD_TIMEOUT);
    let mut response = match agent.get(url).call() {
        Ok(response) => response,
        Err(error) => {
            let _ = fs::remove_file(&part);
            if matches!(error, ureq::Error::Timeout(_)) {
                log("下载被取消或超时。");
            } else {
                log(&format!("下载失败：{error}"));
            }
            return false;
        }
    };

    let status = response.status().as_u16();
    if !(200..300).contains(&status) {
        log(&format!("下载失败：服务器返回 HTTP {status}。"));
        return false;
    }

    let mut fail_reason = String::new();
    let mut total: u64 = 0;
    let mut hasher = Sha256::new();

    let mut file = match fs::File::create(&part) {
        Ok(file) => file,
        Err(error) => {
            log(&format!("下载失败：{error}"));
            return false;
        }
    };

    {
        let mut reader = response.body_mut().as_reader();
        let mut buffer = [0u8; 64 * 1024];
        loop {
            let read = match reader.read(&mut buffer) {
                Ok(0) => break,
                Ok(read) => read,
                Err(error) => {
                    fail_reason = format!("读取更新包失败：{error}");
                    break;
                }
            };
            total += read as u64;
            if total > MAX_INSTALLER_BYTES as u64 {
                fail_reason = "更新包超过允许的最大体积。".to_string();
                break;
            }
            hasher.update(&buffer[..read]);
            if let Err(error) = file.write_all(&buffer[..read]) {
                fail_reason = format!("写入更新包失败：{error}");
                break;
            }
        }
    }

    if fail_reason.is_empty() {
        if let Err(error) = file.flush() {
            fail_reason = format!("写入更新包失败：{error}");
        } else {
            let actual = to_hex(&hasher.finalize());
            if installer.size > 0 && total != installer.size as u64 {
                fail_reason = format!("文件大小 {total} 与清单 {} 不一致。", installer.size);
            } else if !actual.eq_ignore_ascii_case(&expected) {
                fail_reason = "SHA256 不匹配。".to_string();
            }
        }
    }
    drop(file);

    if !fail_reason.is_empty() {
        let _ = fs::remove_file(&part);
        log(&format!("下载校验失败：{fail_reason}"));
        return false;
    }

    if let Err(error) = fs::rename(&part, destination) {
        let _ = fs::remove_file(&part);
        log(&format!("下载失败：{error}"));
        return false;
    }

    // 去掉 Mark-of-the-Web，避免静默启动新版本时被 SmartScreen 拦截。
    crate::selfupdate::remove_zone_identifier(destination);

    log(&format!(
        "下载完成并校验通过：{}（{total} 字节）",
        destination.display()
    ));
    true
}

/// 启动自更新助手：等待当前进程退出后把新 exe 覆盖到自身位置并重启。
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
    let directory = update_directory();
    let _ = fs::create_dir_all(&directory);
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

/// 与旧版本保持同名的门面，便于调用处读起来一致。
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
            format!("{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{{\"url\":\"https://example.com/a.exe\",\"sha256\":\"abc\"}}}}"),
            format!("{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{{\"url\":\"http://example.com/a.exe\",\"sha256\":\"{sha}\"}}}}"),
            format!("{{\"schemaVersion\":1,\"version\":\"9.9.9\",\"installer\":{{\"url\":\"https://example.com/a.exe\",\"sha256\":\"{sha}\"}}}}"),
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

        let agent = build_agent(CHECK_TIMEOUT);
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
