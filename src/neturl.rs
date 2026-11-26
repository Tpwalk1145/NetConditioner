//! 更新地址与哈希的校验。

use std::net::ToSocketAddrs;

/// 更新地址必须是 https；本机回环地址允许 http，便于离线联调。
pub fn try_validate_feed_url(url: &str) -> Result<(), String> {
    let parsed = parse_update_url(url, "更新地址")?;
    if !url_is_public_or_loopback(&parsed, true) {
        return Err("更新地址必须使用 https（本机回环地址可用 http 调试）。".to_string());
    }
    Ok(())
}

fn parse_update_url(url: &str, label: &str) -> Result<url::Url, String> {
    let text = url.trim();
    if text.is_empty() {
        return Err("未配置更新地址。".to_string());
    }
    if text.len() > 2048 {
        return Err("更新地址过长。".to_string());
    }
    let parsed = url::Url::parse(text).map_err(|_| format!("{label}无效。"))?;
    if parsed.host_str().is_none() || parsed.host_str() == Some("") {
        return Err(format!("{label}不是有效的绝对 URL。"));
    }
    // Never put credentials in errors or logs.
    if !parsed.username().is_empty() || parsed.password().is_some() {
        return Err(format!("{label}不能包含用户信息。"));
    }
    if parsed.scheme() != "https" && parsed.scheme() != "http" {
        return Err(format!(
            "{label}必须使用 https（本机回环地址可用 http 调试）。"
        ));
    }
    Ok(parsed)
}

fn url_is_public_or_loopback(url: &url::Url, initial: bool) -> bool {
    if url.scheme() == "https" {
        return true;
    }
    is_loopback(url) && initial
}

pub fn redact_url_for_log(url: &str) -> String {
    let mut parsed = match url::Url::parse(url) {
        Ok(parsed) => parsed,
        Err(_) => return "(无效地址)".to_string(),
    };
    let _ = parsed.set_username("");
    let _ = parsed.set_password(None);
    parsed.set_fragment(None);
    parsed.to_string()
}

/// Validate every redirect before ureq can issue another request. An HTTP
/// development session must remain loopback-only and cannot start from HTTPS.
pub fn validate_redirect(current: &url::Url, next: &url::Url) -> Result<(), String> {
    let next = parse_update_url(next.as_str(), "重定向地址")?;
    let next_loopback = is_loopback(&next);
    if next_loopback && !(current.scheme() == "http" && is_loopback(current)) {
        return Err("重定向到本机回环地址，已拒绝。".to_string());
    }
    if current.scheme() == "http" && !next_loopback {
        return Err("本机调试重定向离开回环地址，已拒绝。".to_string());
    }
    if next.scheme() == "http"
        && !(current.scheme() == "http" && is_loopback(current) && next_loopback)
    {
        return Err("重定向降级或离开本机回环地址，已拒绝。".to_string());
    }
    if let Some(ip) = next
        .host_str()
        .and_then(|host| host.parse::<std::net::IpAddr>().ok())
    {
        if !next_loopback && !is_global_address(ip) {
            return Err("重定向到非公网地址，已拒绝。".to_string());
        }
    }
    if next.scheme() == "https" && next.host_str().is_none() {
        return Err("重定向地址无效。".to_string());
    }
    Ok(())
}

/// Production HTTPS targets must not resolve to private, link-local or other
/// non-global addresses. Loopback HTTP is an explicit development-only path.
pub fn validate_public_target(url: &url::Url, allow_loopback: bool) -> Result<(), String> {
    let Some(host) = url.host_str() else {
        return Err("更新地址无效。".to_string());
    };
    let port = url
        .port_or_known_default()
        .ok_or_else(|| "更新地址端口无效。".to_string())?;
    let addresses = (host, port)
        .to_socket_addrs()
        .map_err(|_| "无法解析更新服务器地址。".to_string())?;
    let mut found = false;
    for address in addresses {
        found = true;
        let ip = address.ip();
        let permitted = if allow_loopback && is_loopback(url) {
            ip.is_loopback()
        } else {
            is_global_address(ip)
        };
        if !permitted {
            return Err("更新服务器解析到非公网地址，已拒绝。".to_string());
        }
    }
    if !found {
        return Err("无法解析更新服务器地址。".to_string());
    }
    Ok(())
}

fn is_global_address(ip: std::net::IpAddr) -> bool {
    match ip {
        std::net::IpAddr::V4(ip) => {
            !ip.is_private()
                && !ip.is_loopback()
                && !ip.is_link_local()
                && !ip.is_unspecified()
                && !ip.is_broadcast()
                && !ip.is_multicast()
                && !ip.is_documentation()
        }
        std::net::IpAddr::V6(ip) => {
            !ip.is_loopback()
                && !ip.is_unique_local()
                && !ip.is_unicast_link_local()
                && !ip.is_unspecified()
                && !ip.is_multicast()
        }
    }
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
    let parsed = parse_update_url(url, "更新包地址")?;
    if parsed.scheme() != "https" && !is_loopback(&parsed) {
        return Err("更新包地址必须使用 https。".to_string());
    }
    Ok(parsed)
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
    // Restrict to ASCII before using byte offsets on the remaining input.
    if !text.is_ascii() {
        return None;
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
    fn redirect_targets_reject_downgrade_userinfo_and_private_addresses() {
        let https = url::Url::parse("https://example.com/feed").unwrap();
        assert!(validate_redirect(
            &https,
            &url::Url::parse("https://cdn.example.com/a").unwrap()
        )
        .is_ok());
        assert!(
            validate_redirect(&https, &url::Url::parse("http://example.com/a").unwrap()).is_err()
        );
        assert!(validate_redirect(
            &https,
            &url::Url::parse("https://user:secret@example.com/a").unwrap()
        )
        .is_err());
        assert!(
            validate_redirect(&https, &url::Url::parse("https://127.0.0.1/a").unwrap()).is_err()
        );
        let local = url::Url::parse("http://127.0.0.1:8080/feed").unwrap();
        assert!(
            validate_redirect(&local, &url::Url::parse("http://localhost:8081/a").unwrap()).is_ok()
        );
        assert!(validate_redirect(
            &local,
            &url::Url::parse("https://localhost:8081/a").unwrap()
        )
        .is_ok());
        assert!(
            validate_redirect(&local, &url::Url::parse("http://192.168.1.2/a").unwrap()).is_err()
        );
        assert!(
            try_validate_feed_url("https://user:secret@example.com/feed")
                .unwrap_err()
                .contains("用户信息")
        );
        assert!(
            !try_validate_feed_url("https://user:secret@example.com/feed")
                .unwrap_err()
                .contains("secret")
        );
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
