//! 全局热键的校验与显示。

use crate::config::{MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN};

pub fn is_valid(modifiers: u32, key: i32) -> bool {
    let _ = modifiers;
    if key == 0 {
        return false;
    }
    !is_modifier_key(key)
}

pub fn is_modifier_key(key: i32) -> bool {
    matches!(
        key,
        0x10 | 0x11 | 0x12 | 0x5B | 0x5C | 0xA0 | 0xA1 | 0xA2 | 0xA3 | 0xA4 | 0xA5
    )
}

pub fn format(modifiers: u32, key: i32) -> String {
    let mut parts: Vec<&str> = Vec::with_capacity(5);
    if modifiers & MOD_CONTROL != 0 {
        parts.push("Ctrl");
    }
    if modifiers & MOD_ALT != 0 {
        parts.push("Alt");
    }
    if modifiers & MOD_SHIFT != 0 {
        parts.push("Shift");
    }
    if modifiers & MOD_WIN != 0 {
        parts.push("Win");
    }

    let name = key_name(key);
    parts.push(&name);
    parts.join(" + ")
}

/// 把虚拟键码转成显示名。
pub fn key_name(key: i32) -> String {
    match key {
        0x08 => "Back".into(),
        0x09 => "Tab".into(),
        0x0C => "Clear".into(),
        0x0D => "Enter".into(),
        0x13 => "Pause".into(),
        0x14 => "Capital".into(),
        0x1B => "Esc".into(),
        0x20 => "Space".into(),
        0x21 => "PageUp".into(),
        0x22 => "PageDown".into(),
        0x23 => "End".into(),
        0x24 => "Home".into(),
        0x25 => "Left".into(),
        0x26 => "Up".into(),
        0x27 => "Right".into(),
        0x28 => "Down".into(),
        0x2C => "Snapshot".into(),
        0x2D => "Insert".into(),
        0x2E => "Delete".into(),
        0x5B => "LWin".into(),
        0x5C => "RWin".into(),
        0x5D => "Apps".into(),
        0x30..=0x39 => ((b'0' + (key - 0x30) as u8) as char).to_string(),
        0x41..=0x5A => ((b'A' + (key - 0x41) as u8) as char).to_string(),
        0x60..=0x69 => format!("NumPad{}", key - 0x60),
        0x6A => "Multiply".into(),
        0x6B => "Add".into(),
        0x6C => "Separator".into(),
        0x6D => "Subtract".into(),
        0x6E => "Decimal".into(),
        0x6F => "Divide".into(),
        0x70..=0x87 => format!("F{}", key - 0x6F),
        0x90 => "NumLock".into(),
        0x91 => "Scroll".into(),
        0xBA => ";".into(),
        0xBB => "=".into(),
        0xBC => ",".into(),
        0xBD => "-".into(),
        0xBE => ".".into(),
        0xBF => "/".into(),
        0xC0 => "`".into(),
        0xDB => "[".into(),
        0xDC => "\\".into(),
        0xDD => "]".into(),
        0xDE => "'".into(),
        _ => key.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validation_matches_original() {
        assert!(is_valid(0, 0x77));
        assert!(is_valid(MOD_CONTROL, 0x55));
        assert!(!is_valid(0, 0x10));
        assert!(!is_valid(0, 0));
    }

    #[test]
    fn formats_hotkeys() {
        assert_eq!(format(0, 0x77), "F8");
        assert_eq!(format(MOD_CONTROL | MOD_ALT, 0x55), "Ctrl + Alt + U");
        assert_eq!(
            format(MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, 0x30),
            "Ctrl + Alt + Shift + Win + 0"
        );
        assert_eq!(format(0, 0x0D), "Enter");
        assert_eq!(format(0, 0x1B), "Esc");
    }
}
