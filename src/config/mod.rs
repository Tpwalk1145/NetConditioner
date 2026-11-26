//! 配置模型与清洗规则。
//!
//! JSON 沿用旧版格式约定：camelCase 属性名、枚举写字符串、缩进输出。

use serde::{Deserialize, Deserializer, Serialize};
use serde_json::Value;
use std::collections::HashSet;

pub mod store;

pub use store::ProfileStore;

/// 目标进程上限。
pub const MAX_PROCESS_COUNT: usize = 64;

// Win32 MOD_* 常量（不含 MOD_NOREPEAT）。
pub const MOD_ALT: u32 = 0x0001;
pub const MOD_CONTROL: u32 = 0x0002;
pub const MOD_SHIFT: u32 = 0x0004;
pub const MOD_WIN: u32 = 0x0008;
pub const MOD_NOREPEAT: u32 = 0x4000;

pub const DEFAULT_HOTKEY_MODIFIERS: u32 = MOD_CONTROL | MOD_ALT;
pub const DEFAULT_HOTKEY_KEY: i32 = 0x55; // VK_U

/// 弱网控制协议，数值与界面显示的 TCP (1) / UDP (2) / ALL (3) 一致。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum ConditionProtocol {
    Tcp,
    Udp,
    #[default]
    All,
}

/// 全局热键。Modifiers 使用 Win32 MOD_* 标志（不含 MOD_NOREPEAT）。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct HotkeySettings {
    pub modifiers: u32,
    pub key: i32,
}

impl Default for HotkeySettings {
    fn default() -> Self {
        Self {
            modifiers: DEFAULT_HOTKEY_MODIFIERS,
            key: DEFAULT_HOTKEY_KEY,
        }
    }
}

/// 单个方向的弱网参数。
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct DirectionProfile {
    pub delay_ms: i32,
    pub jitter_ms: i32,
    pub bandwidth_kbps: i32,
    pub random_loss_percent: i32,
    pub burst_allow_ms: i32,
    pub burst_drop_ms: i32,
}

impl DirectionProfile {
    pub fn is_all_zero(&self) -> bool {
        self.delay_ms == 0
            && self.jitter_ms == 0
            && self.bandwidth_kbps == 0
            && self.random_loss_percent == 0
            && self.burst_allow_ms == 0
            && self.burst_drop_ms == 0
    }

    /// 返回 None 表示合法，否则返回错误描述。
    pub fn validate(&self, direction_name: &str) -> Option<String> {
        if self.delay_ms < 0 || self.delay_ms > 60000 {
            return Some(format!("{direction_name}延时必须在 0 到 60000 ms 之间。"));
        }
        if self.jitter_ms < 0 || self.jitter_ms > 60000 {
            return Some(format!(
                "{direction_name}延时抖动必须在 0 到 60000 ms 之间。"
            ));
        }
        if self.bandwidth_kbps < 0 || self.bandwidth_kbps > 1_000_000 {
            return Some(format!(
                "{direction_name}带宽必须在 0 到 1000000 kbps 之间。"
            ));
        }
        if self.random_loss_percent < 0 || self.random_loss_percent > 100 {
            return Some(format!("{direction_name}随机丢包必须在 0 到 100% 之间。"));
        }
        if self.burst_allow_ms < 0 || self.burst_allow_ms > 600_000 {
            return Some(format!(
                "{direction_name}连续丢包-放行必须在 0 到 600000 ms 之间。"
            ));
        }
        if self.burst_drop_ms < 0 || self.burst_drop_ms > 600_000 {
            return Some(format!(
                "{direction_name}连续丢包-丢包必须在 0 到 600000 ms 之间。"
            ));
        }
        None
    }
}

/// 一套可保存、可切换的弱网配置。
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NetworkProfile {
    pub id: String,
    pub name: String,
    pub uplink: DirectionProfile,
    pub downlink: DirectionProfile,
    pub protocol: ConditionProtocol,
}

impl Default for NetworkProfile {
    fn default() -> Self {
        Self {
            id: new_id(),
            name: "默认配置".to_string(),
            uplink: DirectionProfile::default(),
            downlink: DirectionProfile::default(),
            protocol: ConditionProtocol::All,
        }
    }
}

impl NetworkProfile {
    pub fn is_all_zero(&self) -> bool {
        self.uplink.is_all_zero() && self.downlink.is_all_zero()
    }

    pub fn validate(&self) -> Option<String> {
        if self.name.trim().is_empty() {
            return Some("配置名称不能为空。".to_string());
        }
        if self.name.chars().count() > 60 {
            return Some("配置名称不能超过 60 个字符。".to_string());
        }
        self.uplink
            .validate("上行")
            .or_else(|| self.downlink.validate("下行"))
    }
}

/// 全局设置。
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppSettings {
    /// 0 表示不自动停止。
    pub max_run_minutes: i32,
    /// 除配置延时之外允许的额外排队时延上限；0 表示不限制。
    pub max_queue_delay_ms: i32,
    /// 目标进程文件名（如 chrome.exe）；为空表示全局。
    pub process_names: Vec<String>,
    /// latest.json 地址；为空表示关闭云更新。
    pub update_feed_url: String,
    /// 是否启用全静默自动更新。
    pub auto_update: bool,
    /// 上次检查更新的时间（Unix 秒，UTC）。
    pub last_update_check_unix: i64,
    /// 上次尝试升级的时间（Unix 秒，UTC）。
    pub last_update_attempt_unix: i64,
    /// 上次尝试升级的版本。
    pub last_attempted_version: String,
}

impl Default for AppSettings {
    fn default() -> Self {
        Self {
            max_run_minutes: 0,
            max_queue_delay_ms: 2000,
            process_names: Vec::new(),
            update_feed_url: String::new(),
            auto_update: true,
            last_update_check_unix: 0,
            last_update_attempt_unix: 0,
            last_attempted_version: String::new(),
        }
    }
}

/// 配置文件根对象。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppConfig {
    pub version: i32,
    pub active_profile_id: Option<String>,
    pub profiles: Vec<NetworkProfile>,
    pub hotkey: HotkeySettings,
    pub settings: AppSettings,
    /// Unknown root fields survive a read/save cycle.
    #[serde(flatten)]
    pub extra: std::collections::BTreeMap<String, Value>,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            version: 1,
            active_profile_id: None,
            profiles: Vec::new(),
            hotkey: HotkeySettings::default(),
            settings: AppSettings::default(),
            extra: std::collections::BTreeMap::new(),
        }
    }
}

impl AppConfig {
    /// 新建一份带默认配置的文件内容。
    pub fn fresh() -> Self {
        let mut config = Self::default();
        config.ensure_default_profile();
        config
    }

    pub fn find_active_profile(&self) -> Option<&NetworkProfile> {
        if self.profiles.is_empty() {
            return None;
        }
        let active = self.active_profile_id.as_deref();
        self.profiles
            .iter()
            .find(|profile| Some(profile.id.as_str()) == active)
            .or_else(|| self.profiles.first())
    }

    pub fn ensure_default_profile(&mut self) {
        if self.profiles.is_empty() {
            self.profiles.push(NetworkProfile::default());
        }

        let first_id = self.profiles[0].id.clone();
        let active_ok = self
            .active_profile_id
            .as_deref()
            .is_some_and(|id| self.profiles.iter().any(|profile| profile.id == id));
        if !active_ok {
            self.active_profile_id = Some(first_id);
        }
    }
}

impl<'de> Deserialize<'de> for AppConfig {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        #[derive(Deserialize, Default)]
        #[serde(rename_all = "camelCase", default)]
        struct RawConfig {
            version: Option<i32>,
            active_profile_id: Option<String>,
            profiles: Option<Vec<Option<RawProfile>>>,
            hotkey: Option<Value>,
            settings: Option<Value>,
            #[serde(flatten)]
            extra: std::collections::BTreeMap<String, Value>,
        }

        #[derive(Deserialize, Default)]
        #[serde(rename_all = "camelCase", default)]
        struct RawProfile {
            id: Option<String>,
            #[serde(default = "default_profile_name")]
            name: Option<String>,
            uplink: Option<Value>,
            downlink: Option<Value>,
            protocol: Option<ConditionProtocol>,
        }

        fn default_profile_name() -> Option<String> {
            Some("默认配置".to_string())
        }

        fn field<T: serde::de::DeserializeOwned>(value: &Value, name: &str) -> Option<T> {
            serde_json::from_value(value.get(name).cloned().unwrap_or(Value::Null)).ok()
        }

        fn direction(value: Option<Value>) -> DirectionProfile {
            let Some(value) = value else {
                return DirectionProfile::default();
            };
            DirectionProfile {
                delay_ms: field(&value, "delayMs").unwrap_or(0),
                jitter_ms: field(&value, "jitterMs").unwrap_or(0),
                bandwidth_kbps: field(&value, "bandwidthKbps").unwrap_or(0),
                random_loss_percent: field(&value, "randomLossPercent").unwrap_or(0),
                burst_allow_ms: field(&value, "burstAllowMs").unwrap_or(0),
                burst_drop_ms: field(&value, "burstDropMs").unwrap_or(0),
            }
        }

        let raw = RawConfig::deserialize(deserializer)?;
        let empty = Value::Object(serde_json::Map::new());
        let settings = raw.settings.unwrap_or(empty.clone());
        let settings = AppSettings {
            max_run_minutes: field(&settings, "maxRunMinutes").unwrap_or(0),
            max_queue_delay_ms: field(&settings, "maxQueueDelayMs").unwrap_or(2000),
            process_names: field::<Vec<Value>>(&settings, "processNames")
                .unwrap_or_default()
                .into_iter()
                .filter_map(|value| value.as_str().map(str::to_string))
                .collect(),
            update_feed_url: field(&settings, "updateFeedUrl").unwrap_or_default(),
            auto_update: field(&settings, "autoUpdate").unwrap_or(true),
            last_update_check_unix: field(&settings, "lastUpdateCheckUnix").unwrap_or(0),
            last_update_attempt_unix: field(&settings, "lastUpdateAttemptUnix").unwrap_or(0),
            last_attempted_version: field(&settings, "lastAttemptedVersion").unwrap_or_default(),
        };
        let hotkey = raw.hotkey.unwrap_or(empty);
        let hotkey = HotkeySettings {
            modifiers: field(&hotkey, "modifiers").unwrap_or(DEFAULT_HOTKEY_MODIFIERS),
            key: field(&hotkey, "key").unwrap_or(DEFAULT_HOTKEY_KEY),
        };
        let profiles = raw
            .profiles
            .unwrap_or_default()
            .into_iter()
            .flatten()
            .map(|raw| NetworkProfile {
                id: raw.id.unwrap_or_default(),
                name: raw.name.unwrap_or_default(),
                uplink: direction(raw.uplink),
                downlink: direction(raw.downlink),
                protocol: raw.protocol.unwrap_or_default(),
            })
            .collect();

        Ok(AppConfig {
            version: raw.version.unwrap_or(1),
            active_profile_id: raw.active_profile_id,
            profiles,
            hotkey,
            settings,
            extra: raw.extra,
        })
    }
}

/// 清洗结果。
#[derive(Debug, Default, Clone, Copy)]
pub struct SanitizeOutcome {
    /// 因名称为空被丢弃的配置项数量。
    pub removed_profiles: usize,
}

/// 读取配置后统一清洗。
pub fn sanitize(config: &mut AppConfig, now_unix: i64) -> SanitizeOutcome {
    config.hotkey.modifiers &= MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    if config.hotkey.modifiers == 0 {
        config.hotkey.modifiers = DEFAULT_HOTKEY_MODIFIERS;
    }
    if !(1..=0xff).contains(&config.hotkey.key) {
        config.hotkey.key = DEFAULT_HOTKEY_KEY;
    }
    config.settings.max_run_minutes = config.settings.max_run_minutes.clamp(0, 1440);
    config.settings.max_queue_delay_ms = config.settings.max_queue_delay_ms.clamp(0, 60000);
    config.settings.process_names = sanitize_process_names(&config.settings.process_names);
    config.settings.update_feed_url = sanitize_feed_url(&config.settings.update_feed_url);
    config.settings.last_update_check_unix =
        clamp_unix(config.settings.last_update_check_unix, now_unix);
    config.settings.last_update_attempt_unix =
        clamp_unix(config.settings.last_update_attempt_unix, now_unix);
    config.settings.last_attempted_version =
        crate::version::normalize(&config.settings.last_attempted_version).unwrap_or_default();

    let mut removed = 0usize;
    let mut seen: HashSet<String> = HashSet::with_capacity(config.profiles.len());
    let mut kept: Vec<NetworkProfile> = Vec::with_capacity(config.profiles.len());
    for mut profile in std::mem::take(&mut config.profiles) {
        if profile.name.trim().is_empty() {
            removed += 1;
            continue;
        }

        let lower = profile.id.to_ascii_lowercase();
        if profile.id.trim().is_empty() || !seen.insert(lower) {
            profile.id = new_id();
            seen.insert(profile.id.to_ascii_lowercase());
        }

        clamp_direction(&mut profile.uplink);
        clamp_direction(&mut profile.downlink);
        kept.push(profile);
    }

    config.profiles = kept;
    config.ensure_default_profile();

    SanitizeOutcome {
        removed_profiles: removed,
    }
}

fn clamp_direction(direction: &mut DirectionProfile) {
    direction.delay_ms = direction.delay_ms.clamp(0, 60000);
    direction.jitter_ms = direction.jitter_ms.clamp(0, 60000);
    direction.bandwidth_kbps = direction.bandwidth_kbps.clamp(0, 1_000_000);
    direction.random_loss_percent = direction.random_loss_percent.clamp(0, 100);
    direction.burst_allow_ms = direction.burst_allow_ms.clamp(0, 600_000);
    direction.burst_drop_ms = direction.burst_drop_ms.clamp(0, 600_000);
}

fn clamp_unix(value: i64, now: i64) -> i64 {
    if value < 0 {
        return 0;
    }
    if value > now.saturating_add(86400) {
        return now;
    }
    value
}

fn sanitize_feed_url(raw: &str) -> String {
    let text = raw.trim();
    if text.is_empty() || text.len() > 2048 {
        return String::new();
    }
    if crate::neturl::try_validate_feed_url(text).is_ok() {
        text.to_string()
    } else {
        String::new()
    }
}

/// 生成 32 位小写十六进制 Id。
pub fn new_id() -> String {
    let mut id = String::with_capacity(32);
    for _ in 0..4 {
        id.push_str(&format!("{:08x}", fastrand::u32(..)));
    }
    id
}

/// 把用户输入（可含路径、大小写、.exe）规范成不带扩展名的进程名。
pub fn normalize_process_name(name: &str) -> String {
    let trimmed = name.trim();
    if trimmed.is_empty() {
        return String::new();
    }

    let mut file_name = trimmed.rsplit(['\\', '/']).next().unwrap_or("").trim();

    if let Some(stem) = file_name.get(file_name.len().saturating_sub(4)..) {
        if stem.eq_ignore_ascii_case(".exe") {
            file_name = &file_name[..file_name.len() - 4];
        }
    }

    file_name.trim().to_lowercase()
}

/// 配置和界面里保存、显示的文件名，如 chrome.exe。
pub fn normalize_display_name(name: &str) -> String {
    let normalized = normalize_process_name(name);
    if normalized.is_empty() {
        String::new()
    } else {
        format!("{normalized}.exe")
    }
}

/// 规范化并去重（忽略大小写），最多保留 64 个。
pub fn sanitize_process_names(names: &[String]) -> Vec<String> {
    let mut result: Vec<String> = Vec::new();
    for raw in names {
        let name = normalize_display_name(raw);
        if name.is_empty() {
            continue;
        }
        if result
            .iter()
            .any(|existing| existing.eq_ignore_ascii_case(&name))
        {
            continue;
        }
        result.push(name);
        if result.len() >= MAX_PROCESS_COUNT {
            break;
        }
    }
    result
}

pub fn now_unix() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_secs() as i64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    const DOTNET_CONFIG: &str = r#"{
      "version": 1,
      "activeProfileId": "abc",
      "profiles": [
        {
          "id": "p1",
          "name": "默认配置",
          "uplink": { "delayMs": 300, "jitterMs": 100, "bandwidthKbps": 64, "randomLossPercent": 50, "burstAllowMs": 1000, "burstDropMs": 2000 },
          "downlink": {},
          "protocol": "Tcp"
        },
        null,
        { "id": "p2", "name": "  ", "protocol": "Udp" }
      ],
      "hotkey": { "modifiers": 3, "key": 85 },
      "settings": {
        "maxRunMinutes": 9999,
        "maxQueueDelayMs": -5,
        "processNames": ["Chrome.EXE", "C:\\Tools\\MyApp.exe", "chrome.exe"],
        "updateFeedUrl": "http://example.com/latest.json",
        "autoUpdate": true,
        "lastUpdateCheckUnix": 99999999999999,
        "lastAttemptedVersion": "1.2"
      }
    }"#;

    #[test]
    fn reads_dotnet_config_and_sanitizes() {
        let mut config: AppConfig = serde_json::from_str(DOTNET_CONFIG).expect("parse");
        let outcome = sanitize(&mut config, 1_700_000_000);

        assert_eq!(outcome.removed_profiles, 1);
        assert_eq!(config.profiles.len(), 1);
        assert_eq!(config.profiles[0].name, "默认配置");
        assert_eq!(config.profiles[0].protocol, ConditionProtocol::Tcp);
        assert_eq!(config.profiles[0].uplink.delay_ms, 300);
        assert_eq!(config.profiles[0].uplink.random_loss_percent, 50);
        assert_eq!(config.active_profile_id.as_deref(), Some("p1"));

        assert_eq!(config.settings.max_run_minutes, 1440);
        assert_eq!(config.settings.max_queue_delay_ms, 0);
        assert_eq!(
            config.settings.process_names,
            vec!["chrome.exe", "myapp.exe"]
        );
        assert_eq!(config.settings.update_feed_url, "");
        assert!(config.settings.last_update_check_unix <= 1_700_000_000);
        assert_eq!(config.settings.last_attempted_version, "1.2.0");
    }

    #[test]
    fn defaults_and_null_handling() {
        let mut config: AppConfig = serde_json::from_str(
            r#"{"settings": null, "hotkey": null, "profiles": [{"name": null}]}"#,
        )
        .expect("parse");
        let outcome = sanitize(&mut config, 0);

        assert_eq!(outcome.removed_profiles, 1);
        assert_eq!(config.profiles.len(), 1);
        assert_eq!(config.profiles[0].name, "默认配置");
        assert_eq!(config.settings.max_queue_delay_ms, 2000);
        assert!(config.settings.auto_update);
        assert_eq!(config.hotkey.modifiers, DEFAULT_HOTKEY_MODIFIERS);
        assert_eq!(config.hotkey.key, DEFAULT_HOTKEY_KEY);
    }

    #[test]
    fn serializes_like_dotnet() {
        let config = AppConfig::fresh();
        let json = serde_json::to_string_pretty(&config).expect("serialize");
        assert!(json.contains("\"activeProfileId\""));
        assert!(json.contains("\"delayMs\""));
        assert!(json.contains("\"protocol\": \"All\""));
        assert!(json.contains("\"maxQueueDelayMs\": 2000"));
        assert!(json.contains("\"autoUpdate\": true"));
        assert!(json.contains("\"processNames\""));
    }

    #[test]
    fn duplicate_ids_get_regenerated() {
        let mut config: AppConfig =
            serde_json::from_str(r#"{"profiles":[{"id":"x","name":"a"},{"id":"x","name":"b"}]}"#)
                .expect("parse");
        sanitize(&mut config, 0);
        assert_eq!(config.profiles.len(), 2);
        assert_ne!(config.profiles[0].id, config.profiles[1].id);
    }

    #[test]
    fn unicode_process_names_are_character_safe() {
        assert_eq!(normalize_process_name("应用.EXE"), "应用");
        assert_eq!(normalize_process_name("ééé.ExE"), "ééé");
        assert_eq!(normalize_process_name("C:\\工具\\应用.exe"), "应用");
        assert_eq!(normalize_display_name("ééé"), "ééé.exe");
        assert_eq!(normalize_process_name("应用"), "应用");
    }

    #[test]
    fn invalid_hotkey_is_sanitized() {
        let mut config: AppConfig = serde_json::from_str(
            r#"{"hotkey":{"modifiers":-1,"key":0},"settings":{"maxRunMinutes":"bad","processNames":["ok.exe",3,null]}}"#,
        ).expect("parse");
        sanitize(&mut config, 0);
        assert_eq!(config.hotkey.modifiers, DEFAULT_HOTKEY_MODIFIERS);
        assert_eq!(config.hotkey.key, DEFAULT_HOTKEY_KEY);
        assert_eq!(config.settings.process_names, vec!["ok.exe"]);
        config.hotkey.modifiers = MOD_NOREPEAT;
        config.hotkey.key = 300;
        sanitize(&mut config, 0);
        assert_eq!(config.hotkey.modifiers, DEFAULT_HOTKEY_MODIFIERS);
        assert_eq!(config.hotkey.key, DEFAULT_HOTKEY_KEY);
    }

    #[test]
    fn malformed_individual_fields_do_not_discard_whole_config() {
        let config: AppConfig = serde_json::from_str(
            r#"{"version":1,"future":7,"profiles":[{"id":"p","name":"kept","uplink":{"delayMs":"bad","jitterMs":9}}],"settings":{"maxRunMinutes":"bad","futureField":true}}"#,
        ).expect("parse");
        assert_eq!(config.profiles[0].name, "kept");
        assert_eq!(config.profiles[0].uplink.delay_ms, 0);
        assert_eq!(config.profiles[0].uplink.jitter_ms, 9);
        assert_eq!(config.settings.max_run_minutes, 0);
        assert_eq!(config.extra["future"], 7);
    }

    #[test]
    fn process_names_are_deduplicated_and_capped() {
        let mut names: Vec<String> = (0..80).map(|index| format!("App{index}.EXE")).collect();
        names.push("app0.exe".to_string());
        let sanitized = sanitize_process_names(&names);
        assert_eq!(sanitized.len(), MAX_PROCESS_COUNT);
        assert_eq!(sanitized[0], "app0.exe");
        assert_eq!(
            sanitized.iter().filter(|name| *name == "app0.exe").count(),
            1
        );
    }
}
