//! 配置文件的读取、校验与原子写入。

use std::fs;
use std::path::PathBuf;

use crate::config::{self, AppConfig};

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
        let base = std::env::var_os("APPDATA")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("."));
        base.join("NetConditioner").join("config.json")
    }

    pub fn load(&self) -> LoadResult {
        if !self.path.exists() {
            let mut fresh = AppConfig::fresh();
            let mut outcome_warning = None;
            config::sanitize(&mut fresh, config::now_unix());
            if let Err(error) = self.save(&fresh) {
                outcome_warning = Some(format!("保存配置失败：{error}"));
            }
            return LoadResult {
                config: fresh,
                warning: outcome_warning,
            };
        }

        let text = match fs::read_to_string(&self.path) {
            Ok(text) => text,
            Err(error) => return self.recover(format!("{error}")),
        };

        let mut config: AppConfig = match serde_json::from_str(&text) {
            Ok(config) => config,
            Err(error) => return self.recover(error.to_string()),
        };

        let outcome = config::sanitize(&mut config, config::now_unix());
        let mut warning = if outcome.removed_profiles > 0 {
            Some(format!(
                "已忽略 {} 个损坏的配置项。",
                outcome.removed_profiles
            ))
        } else {
            None
        };

        if let Err(error) = self.save(&config) {
            let message = format!("保存配置失败：{error}");
            warning = Some(match warning {
                Some(existing) => format!("{existing}\r\n{message}"),
                None => message,
            });
        }

        LoadResult { config, warning }
    }

    pub fn save(&self, config: &AppConfig) -> Result<(), String> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }

        let json = serde_json::to_string_pretty(config).map_err(|error| error.to_string())?;
        let mut temp_path = self.path.clone().into_os_string();
        temp_path.push(".tmp");
        let temp_path = PathBuf::from(temp_path);

        fs::write(&temp_path, json).map_err(|error| error.to_string())?;
        fs::rename(&temp_path, &self.path).map_err(|error| {
            let _ = fs::remove_file(&temp_path);
            error.to_string()
        })?;
        Ok(())
    }

    fn recover(&self, error: String) -> LoadResult {
        self.try_backup_broken_config();
        let mut fresh = AppConfig::fresh();
        config::sanitize(&mut fresh, config::now_unix());
        let warning = format!("配置文件读取失败，已恢复默认配置。\r\n{error}");
        let warning = match self.save(&fresh) {
            Ok(()) => warning,
            Err(save_error) => format!("{warning}\r\n保存配置失败：{save_error}"),
        };
        LoadResult {
            config: fresh,
            warning: Some(warning),
        }
    }

    fn try_backup_broken_config(&self) {
        let stamp = chrono::Local::now().format("%Y%m%d-%H%M%S");
        let mut backup = self.path.clone().into_os_string();
        backup.push(format!(".bad-{stamp}"));
        let _ = fs::copy(&self.path, PathBuf::from(backup));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("nc-store-{tag}-{}", crate::config::new_id()));
        std::fs::create_dir_all(&dir).expect("temp dir");
        dir
    }

    #[test]
    fn broken_config_is_backed_up_and_recovered() {
        let dir = temp_dir("broken");
        let path = dir.join("config.json");
        std::fs::write(&path, "{ not json").expect("write");

        let store = ProfileStore::new(path.clone());
        let loaded = store.load();
        assert!(loaded.warning.is_some());
        assert!(!loaded.config.profiles.is_empty());

        let has_backup = std::fs::read_dir(&dir)
            .expect("read dir")
            .flatten()
            .any(|entry| {
                entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with("config.json.bad-")
            });
        assert!(has_backup);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn round_trip_keeps_fields() {
        let dir = temp_dir("roundtrip");
        let path = dir.join("config.json");
        let store = ProfileStore::new(path);

        let mut config = AppConfig::fresh();
        config.settings.max_queue_delay_ms = 500;
        config.settings.auto_update = false;
        store.save(&config).expect("save");

        let loaded = store.load();
        assert_eq!(loaded.config.settings.max_queue_delay_ms, 500);
        assert!(!loaded.config.settings.auto_update);
        assert_eq!(loaded.config.profiles.len(), 1);

        let _ = std::fs::remove_dir_all(&dir);
    }
}
