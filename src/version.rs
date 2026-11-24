//! 程序版本号（来自 Cargo.toml 的 version，编译期确定）。

use std::sync::OnceLock;

/// 4 段数字版本，与 .NET 的 `System.Version` 语义一致。
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Version(pub u32, pub u32, pub u32, pub u32);

impl Version {
    /// 形如 `1.1.0`（第 4 段为 0 时省略）。
    pub fn format(self) -> String {
        let mut text = format!("{}.{}.{}", self.0, self.1, self.2);
        if self.3 > 0 {
            text.push_str(&format!(".{}", self.3));
        }
        text
    }
}

impl std::fmt::Display for Version {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.format())
    }
}

static CURRENT: OnceLock<Version> = OnceLock::new();

pub fn current() -> Version {
    *CURRENT.get_or_init(|| parse(env!("CARGO_PKG_VERSION")).unwrap_or(Version(0, 0, 0, 0)))
}

/// 当前版本，形如 `1.1.0`。
pub fn display() -> String {
    current().format()
}

/// 接受 2~4 段数字，统一补成 4 段；等价于 `AppVersion.TryParse`。
pub fn parse(text: &str) -> Option<Version> {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return None;
    }

    let parts: Vec<&str> = trimmed.split('.').collect();
    if parts.len() < 2 || parts.len() > 4 {
        return None;
    }

    let mut numbers = [0u32; 4];
    for (index, part) in parts.iter().enumerate() {
        if part.is_empty() || !part.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        let value: u32 = part.parse().ok()?;
        // .NET 用 int 解析，超出范围视为无效。
        if value > i32::MAX as u32 {
            return None;
        }
        numbers[index] = value;
    }

    Some(Version(numbers[0], numbers[1], numbers[2], numbers[3]))
}

pub fn is_newer(current: Version, candidate: Version) -> bool {
    candidate > current
}

/// 规范化为 4 段字符串；无效时返回 None（`AppVersion.Normalize`）。
pub fn normalize(text: &str) -> Option<String> {
    parse(text).map(|version| version.format())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_and_compares() {
        assert_eq!(parse("1.0.1"), Some(Version(1, 0, 1, 0)));
        assert_eq!(parse("1.0"), Some(Version(1, 0, 0, 0)));
        assert_eq!(parse("1.0.0.5"), Some(Version(1, 0, 0, 5)));
        assert_eq!(parse(" 2.3.4 "), Some(Version(2, 3, 4, 0)));
        assert_eq!(parse("1.0.0-beta"), None);
        assert_eq!(parse("1"), None);
        assert_eq!(parse("1..2"), None);
        assert_eq!(parse("99999999999.0"), None);
        assert_eq!(parse(""), None);

        assert!(is_newer(Version(1, 0, 0, 0), Version(1, 0, 1, 0)));
        assert!(!is_newer(Version(1, 0, 1, 0), Version(1, 0, 0, 0)));
        assert!(!is_newer(Version(1, 0, 1, 0), Version(1, 0, 1, 0)));
    }

    #[test]
    fn formats_like_dotnet() {
        assert_eq!(Version(1, 1, 0, 0).format(), "1.1.0");
        assert_eq!(Version(1, 1, 0, 7).format(), "1.1.0.7");
        assert_eq!(normalize("1.2"), Some("1.2.0".to_string()));
        assert_eq!(normalize("x"), None);
        assert_eq!(display(), current().format());
    }
}
