//! --check-update 诊断命令。

use std::io::Write;

use crate::config::ProfileStore;
use crate::update::{CheckStatus, UpdateService};
use crate::version;

pub fn run() -> i32 {
    let base = crate::config::store::absolute_env_dir("TEMP").unwrap_or_else(std::env::temp_dir);
    let log_path = base.join(format!(
        "NetConditioner-update-check-{}.log",
        crate::config::new_id()
    ));
    let mut lines: Vec<String> = Vec::new();
    let mut push = |text: String| {
        lines.push(format!(
            "[{}] {text}",
            chrono::Local::now().format("%H:%M:%S")
        ));
    };

    let mut exit_code = 1;
    let store = ProfileStore::with_default_path();
    let loaded = store.load();
    let feed_url = loaded.config.settings.update_feed_url.clone();

    push(format!("当前版本：{}", version::display()));
    push(format!(
        "更新地址：{}",
        if feed_url.trim().is_empty() {
            "(未配置)"
        } else {
            feed_url.as_str()
        }
    ));

    if feed_url.trim().is_empty() {
        push("结果：未配置更新地址，云更新已关闭。".to_string());
    } else {
        match UpdateService::check(&feed_url, version::current()) {
            CheckStatus::UpdateAvailable(manifest, _) => {
                push(format!(
                    "结果：发现新版本 {}。",
                    manifest.version.clone().unwrap_or_default()
                ));
                push(format!(
                    "更新包：{}",
                    manifest
                        .installer
                        .as_ref()
                        .and_then(|installer| installer.url.clone())
                        .unwrap_or_default()
                ));
                push(format!(
                    "SHA256：{}",
                    manifest
                        .installer
                        .as_ref()
                        .and_then(|installer| installer.sha256.clone())
                        .unwrap_or_default()
                ));
                exit_code = 0;
            }
            CheckStatus::UpToDate => {
                push("结果：已是最新版本。".to_string());
                exit_code = 0;
            }
            CheckStatus::NotConfigured(error) => {
                push(format!("结果：未配置更新地址 - {error}"));
            }
            CheckStatus::Failed(error) => {
                push(format!("结果：检查失败 - {error}"));
            }
        }
    }

    let text = lines.join("\r\n") + "\r\n";
    if let Ok(mut file) = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&log_path)
    {
        let _ = file.write_all(text.as_bytes());
        let _ = file.sync_all();
    }

    exit_code
}
