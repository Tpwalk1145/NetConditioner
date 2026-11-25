//! 全静默云更新状态机。

use std::path::PathBuf;
use std::sync::mpsc::Sender;

use crate::config::{self, AppSettings};
use crate::update::{CheckStatus, UpdateManifest, UpdateService};
use crate::version::Version;

const CHECK_INTERVAL_SECONDS: i64 = 24 * 60 * 60;
const RETRY_AFTER_FAILURE_SECONDS: i64 = 24 * 60 * 60;

#[derive(Debug, Default, Clone)]
pub struct UpdateSettingsSnapshot {
    pub last_update_attempt_unix: i64,
    pub last_attempted_version: String,
}

#[derive(Debug, Default)]
pub struct UpdaterState {
    pub busy: bool,
    pub install_started: bool,
    pub deferred_logged: bool,
    pub pending: Option<(UpdateManifest, PathBuf)>,
}

pub enum UpdateEvent {
    Tick(TickOutcome),
}

pub enum TickOutcome {
    CheckFailed {
        error: String,
    },
    UpToDate {
        now: i64,
    },
    Ready {
        manifest: UpdateManifest,
        installer: PathBuf,
        now: i64,
    },
    DownloadFailed {
        manifest: UpdateManifest,
        now: i64,
    },
}

pub fn should_check(settings: &AppSettings, now: i64) -> bool {
    if settings.last_update_check_unix <= 0 {
        return true;
    }
    // 时钟回拨时重新检查。
    if settings.last_update_check_unix > now {
        return true;
    }
    now - settings.last_update_check_unix >= CHECK_INTERVAL_SECONDS
}

fn retry_blocked(snapshot: &UpdateSettingsSnapshot, version: &Option<String>, now: i64) -> bool {
    let Some(version) = version else {
        return false;
    };
    if !snapshot
        .last_attempted_version
        .eq_ignore_ascii_case(version)
    {
        return false;
    }
    if snapshot.last_update_attempt_unix <= 0 {
        return false;
    }
    now - snapshot.last_update_attempt_unix < RETRY_AFTER_FAILURE_SECONDS
}

pub fn spawn_tick(
    feed_url: String,
    current: Version,
    snapshot: UpdateSettingsSnapshot,
    sender: Sender<UpdateEvent>,
    wake: Box<dyn Fn() + Send + Sync>,
) {
    std::thread::spawn(move || {
        let outcome = run_tick(&feed_url, current, &snapshot);
        let _ = sender.send(UpdateEvent::Tick(outcome));
        wake();
    });
}

fn run_tick(feed_url: &str, current: Version, snapshot: &UpdateSettingsSnapshot) -> TickOutcome {
    UpdateService::log(&format!("开始检查更新：{feed_url}"));
    let now = config::now_unix();

    match crate::update::check(feed_url, current) {
        CheckStatus::NotConfigured(error) | CheckStatus::Failed(error) => {
            TickOutcome::CheckFailed { error }
        }
        CheckStatus::UpToDate => TickOutcome::UpToDate { now },
        CheckStatus::UpdateAvailable(manifest, _version) => {
            let version_text = manifest.version.clone().unwrap_or_default();
            if retry_blocked(snapshot, &manifest.version, now) {
                UpdateService::log(&format!(
                    "版本 {version_text} 最近尝试升级失败，24 小时后再试。"
                ));
                return TickOutcome::UpToDate { now };
            }

            UpdateService::log(&format!("发现新版本 {version_text}，开始下载。"));
            let destination = UpdateService::payload_path(&version_text);
            if !UpdateService::download(&manifest, &destination) {
                return TickOutcome::DownloadFailed { manifest, now };
            }

            UpdateService::log(&format!(
                "版本 {version_text} 已下载并校验，等待空闲时安装。"
            ));
            TickOutcome::Ready {
                manifest,
                installer: destination,
                now,
            }
        }
    }
}
