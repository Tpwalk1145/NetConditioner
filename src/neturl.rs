//! 更新地址与哈希的校验。

/// 更新地址必须是 https；本机回环地址允许 http，便于离线联调。
pub fn try_validate_feed_url(url: &str) -> Result<(), String> {
    if url.trim().is_empty() {
        return Err("未配置更新地址。".to_string());
    }

    let text = url.trim();
    if text.len() > 2048 {
        return Err("更新地址过长。".to_string());
    }

    let parsed = match url::Url::parse(text) {
        Ok(parsed) if !parsed.host_str().unwrap_or("").is_empty() => parsed,
        _ => return Err("更新地址不是有效的绝对 URL。".to_string()),
    };

    if parsed.scheme() == "https" {
        return Ok(());
    }

    if parsed.scheme() == "http" && is_loopback(&parsed) {
        return Ok(());
    }

    Err("更新地址必须使用 https（本机回环地址可用 http 调试）。".to_string())
}

/// 本机回环地址：localhost、127.0.0.0/8、::1。
pub fn is_loopback(url: &url::Url) -> bool {
    let host = match url.host_str() {
        Some(host) => host.trim_matches(['[', ']']),
        None => return false,
    };

    if host.eq_ignore_ascii_case("localhost") {
        return true;
    }

    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
        return ip.is_loopback();
    }

    false
}

/// 更新包地址：必须 https，或本机回环 http。
pub fn validate_installer_url(url: &str) -> Result<url::Url, String> {
    let parsed = url::Url::parse(url).map_err(|_| "更新包地址无效。".to_string())?;
    match parsed.scheme() {
        "https" => Ok(parsed),
        "http" if is_loopback(&parsed) => Ok(parsed),
        _ => Err("更新包地址必须使用 https。".to_string()),
    }
}

/// 规范化 SHA256：允许 `sha256:` 前缀，返回大写十六进制。
pub fn try_normalize_sha256(value: &str) -> Option<String> {
    let mut text = value.trim();
    if text.is_empty() {
        return None;
    }

    if text
        .get(..7)
        .is_some_and(|prefix| prefix.eq_ignore_ascii_case("sha256:"))
    {
        text = text[7..].trim();
    }

    if text.len() != 64 || !text.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }

    Some(text.to_ascii_uppercase())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn feed_url_rules() {
        assert!(try_validate_feed_url("https://example.com/latest.json").is_ok());
        assert!(try_validate_feed_url("http://127.0.0.1:8080/latest.json").is_ok());
        assert!(try_validate_feed_url("http://localhost:8080/latest.json").is_ok());
        assert!(try_validate_feed_url("http://example.com/latest.json").is_err());
        assert!(try_validate_feed_url("ftp://example.com/latest.json").is_err());
        assert!(try_validate_feed_url("not a url").is_err());
        assert!(try_validate_feed_url("").is_err());
        assert_eq!(try_validate_feed_url("").unwrap_err(), "未配置更新地址。");
        let long = format!("https://example.com/{}", "a".repeat(2048));
        assert_eq!(try_validate_feed_url(&long).unwrap_err(), "更新地址过长。");
    }

    #[test]
    fn sha256_normalization() {
        let sha = "a".repeat(64);
        assert_eq!(try_normalize_sha256(&sha), Some(sha.to_uppercase()));
        assert_eq!(
            try_normalize_sha256(&format!("sha256: {sha}")),
            Some(sha.to_uppercase())
        );
        assert_eq!(try_normalize_sha256("abc"), None);
        assert_eq!(try_normalize_sha256(&"z".repeat(64)), None);
    }
}
