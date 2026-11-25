//! 自绘控件、字体与通用子控件封装。

use std::collections::HashMap;

use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::System::SystemServices::*;
use windows_sys::Win32::UI::Controls::*;
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::win;

// ---------------------------------------------------------------------------
// 配色
// ---------------------------------------------------------------------------

pub const ACCENT: u32 = 0x00C0_6515; // #1565C0
pub const SURFACE: u32 = 0x00FC_F7F3; // #F3F7FC
pub const CARD: u32 = 0x00FF_FFFF;
pub const TEXT_DARK: u32 = 0x0024_2120; // #202124
pub const TEXT_MUTED: u32 = 0x0070_6B68; // #686B70
pub const GREEN: u32 = 0x0032_7D2E; // #2E7D32
pub const RED: u32 = 0x0028_28C6; // #C62828
pub const ORANGE: u32 = 0x0000_6CEF; // #EF6C00
pub const BORDER: u32 = 0x00CF_CBC8; // #C8CBCF
pub const TEXT_BUTTON: u32 = 0x0043_403C; // #3C4043
pub const SUBTITLE_TEXT: u32 = 0x00FB_DEBB; // #BBDEFB
pub const WARNING_TEXT: u32 = 0x0000_577E; // #7E5700
pub const DANGER_BG: u32 = 0x00EE_EBFF; // #FFEBEE
pub const DANGER_TEXT: u32 = 0x001C_1CB7; // #B71C1C
pub const DANGER_BORDER: u32 = 0x009A_9AEF; // #EF9A9A
pub const GRAY: u32 = 0x009E_9E9E; // #9E9E9E，用于禁用按钮

// ---------------------------------------------------------------------------
// 字体
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FontRole {
    /// 17pt 粗体
    Header,
    /// 8.5pt
    Subtitle,
    /// 16pt 粗体
    Status,
    /// 28pt（状态圆点）
    Dot,
    /// 9pt
    Body,
    /// 9pt 粗体
    BodyBold,
    /// 8pt
    Small,
    /// 8.5pt
    Small85,
    /// 14pt 粗体
    Title,
    /// 12.5pt 粗体
    Toggle,
    /// 11pt 粗体
    Hotkey,
    /// 9pt 下划线（链接）
    Link,
    /// 8.5pt 下划线（链接）
    Link85,
}

pub struct Fonts {
    fonts: HashMap<FontRole, HFONT>,
}

impl Fonts {
    pub fn new(dpi: u32) -> Self {
        let mut fonts: HashMap<FontRole, HFONT> = HashMap::new();
        for (role, family, size, bold, underline) in [
            (FontRole::Header, "Microsoft YaHei UI", 170u32, true, false),
            (FontRole::Subtitle, "Microsoft YaHei UI", 85, false, false),
            (FontRole::Status, "Microsoft YaHei UI", 160, true, false),
            (FontRole::Dot, "Segoe UI", 280, false, false),
            (FontRole::Body, "Microsoft YaHei UI", 90, false, false),
            (FontRole::BodyBold, "Microsoft YaHei UI", 90, true, false),
            (FontRole::Small, "Microsoft YaHei UI", 80, false, false),
            (FontRole::Small85, "Microsoft YaHei UI", 85, false, false),
            (FontRole::Title, "Microsoft YaHei UI", 140, true, false),
            (FontRole::Toggle, "Microsoft YaHei UI", 125, true, false),
            (FontRole::Hotkey, "Microsoft YaHei UI", 110, true, false),
            (FontRole::Link, "Microsoft YaHei UI", 90, false, true),
            (FontRole::Link85, "Microsoft YaHei UI", 85, false, true),
        ] {
            fonts.insert(role, win::create_font(family, size, dpi, bold, underline));
        }
        Self { fonts }
    }

    pub fn get(&self, role: FontRole) -> HFONT {
        *self.fonts.get(&role).unwrap_or(&std::ptr::null_mut())
    }

    pub fn destroy(&mut self) {
        for font in self.fonts.values() {
            win::delete_object(*font);
        }
        self.fonts.clear();
    }
}

// ---------------------------------------------------------------------------
// 自绘控件
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Align {
    Left,
    Center,
}

impl Align {
    fn flags(self) -> u32 {
        match self {
            Align::Left => DT_LEFT,
            Align::Center => DT_CENTER,
        }
    }
}

pub enum WidgetKind {
    Panel {
        bg: u32,
        border: Option<u32>,
    },
    Text {
        text: String,
        font: FontRole,
        color: u32,
        bg: u32,
        align: Align,
        multiline: bool,
        vcenter: bool,
    },
    Button {
        bg: u32,
        fg: u32,
        font: FontRole,
        disabled_bg: u32,
        border: Option<u32>,
    },
}

pub struct Widget {
    pub kind: WidgetKind,
}

pub fn create_panel(
    parent: HWND,
    id: usize,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    bg: u32,
    border: Option<u32>,
    widgets: &mut HashMap<isize, Widget>,
) -> HWND {
    let hwnd = win::create_child(
        parent,
        "STATIC",
        "",
        SS_OWNERDRAW | WS_CHILD | WS_VISIBLE,
        0,
        x,
        y,
        width,
        height,
        id,
    );
    widgets.insert(
        hwnd as isize,
        Widget {
            kind: WidgetKind::Panel { bg, border },
        },
    );
    hwnd
}

#[allow(clippy::too_many_arguments)]
pub fn create_text(
    parent: HWND,
    id: usize,
    text: &str,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    font: FontRole,
    color: u32,
    bg: u32,
    align: Align,
    multiline: bool,
    widgets: &mut HashMap<isize, Widget>,
) -> HWND {
    create_text_full(
        parent, id, text, x, y, width, height, font, color, bg, align, multiline, false, widgets,
    )
}

#[allow(clippy::too_many_arguments)]
pub fn create_text_full(
    parent: HWND,
    id: usize,
    text: &str,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    font: FontRole,
    color: u32,
    bg: u32,
    align: Align,
    multiline: bool,
    vcenter: bool,
    widgets: &mut HashMap<isize, Widget>,
) -> HWND {
    let hwnd = win::create_child(
        parent,
        "STATIC",
        "",
        SS_OWNERDRAW | WS_CHILD | WS_VISIBLE,
        0,
        x,
        y,
        width,
        height,
        id,
    );
    widgets.insert(
        hwnd as isize,
        Widget {
            kind: WidgetKind::Text {
                text: text.to_string(),
                font,
                color,
                bg,
                align,
                multiline,
                vcenter,
            },
        },
    );
    hwnd
}

#[allow(clippy::too_many_arguments)]
pub fn create_button(
    parent: HWND,
    id: usize,
    text: &str,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    bg: u32,
    fg: u32,
    font: FontRole,
    disabled_bg: u32,
    border: Option<u32>,
    link: bool,
    widgets: &mut HashMap<isize, Widget>,
) -> HWND {
    let mut style = (BS_OWNERDRAW as u32) | WS_CHILD | WS_VISIBLE;
    if !link {
        style |= WS_TABSTOP;
    }
    let hwnd = win::create_child(parent, "BUTTON", text, style, 0, x, y, width, height, id);
    widgets.insert(
        hwnd as isize,
        Widget {
            kind: WidgetKind::Button {
                bg,
                fg,
                font,
                disabled_bg,
                border,
            },
        },
    );
    hwnd
}

pub fn create_group(
    parent: HWND,
    id: usize,
    title: &str,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
) -> HWND {
    win::create_child(
        parent,
        "BUTTON",
        title,
        (BS_GROUPBOX as u32) | WS_CHILD | WS_VISIBLE,
        0,
        x,
        y,
        width,
        height,
        id,
    )
}

#[allow(clippy::too_many_arguments)]
pub fn create_edit(
    parent: HWND,
    id: usize,
    text: &str,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    read_only: bool,
    align: Align,
    cue: Option<&str>,
) -> HWND {
    let mut style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (ES_AUTOHSCROLL as u32);
    if read_only {
        style |= ES_READONLY as u32;
    }
    match align {
        Align::Center => style |= ES_CENTER as u32,
        Align::Left => {}
    }
    let hwnd = win::create_child(
        parent,
        "EDIT",
        text,
        style,
        WS_EX_CLIENTEDGE,
        x,
        y,
        width,
        height,
        id,
    );
    if let Some(cue) = cue {
        win::set_cue_banner(hwnd, cue);
    }
    hwnd
}

pub fn create_combo(parent: HWND, id: usize, x: i32, y: i32, width: i32, height: i32) -> HWND {
    win::create_child(
        parent,
        "COMBOBOX",
        "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | (CBS_DROPDOWNLIST as u32),
        0,
        x,
        y,
        width,
        height,
        id,
    )
}

pub fn set_widget_text(widgets: &mut HashMap<isize, Widget>, hwnd: HWND, text: &str) {
    if let Some(widget) = widgets.get_mut(&(hwnd as isize)) {
        if let WidgetKind::Text { text: stored, .. } = &mut widget.kind {
            *stored = text.to_string();
        }
    }
    win::invalidate(hwnd);
}

pub fn set_widget_text_color(widgets: &mut HashMap<isize, Widget>, hwnd: HWND, color: u32) {
    if let Some(widget) = widgets.get_mut(&(hwnd as isize)) {
        if let WidgetKind::Text { color: stored, .. } = &mut widget.kind {
            *stored = color;
        }
    }
    win::invalidate(hwnd);
}

pub fn set_widget_button_colors(
    widgets: &mut HashMap<isize, Widget>,
    hwnd: HWND,
    bg: u32,
    fg: u32,
) {
    if let Some(widget) = widgets.get_mut(&(hwnd as isize)) {
        if let WidgetKind::Button {
            bg: stored_bg,
            fg: stored_fg,
            ..
        } = &mut widget.kind
        {
            *stored_bg = bg;
            *stored_fg = fg;
        }
    }
    win::invalidate(hwnd);
}

pub fn apply_fonts(widgets: &HashMap<isize, Widget>, fonts: &Fonts) {
    for (key, widget) in widgets {
        let role = match &widget.kind {
            WidgetKind::Text { font, .. } => *font,
            WidgetKind::Button { font, .. } => *font,
            WidgetKind::Panel { .. } => continue,
        };
        win::set_font(*key as HWND, fonts.get(role));
    }
}

/// 绘制 SS_OWNERDRAW 控件（面板/文字）与 BS_OWNERDRAW 按钮。
pub fn draw_item(dis: &DRAWITEMSTRUCT, widgets: &HashMap<isize, Widget>, fonts: &Fonts) {
    let hwnd = dis.hwndItem;
    let Some(widget) = widgets.get(&(hwnd as isize)) else {
        return;
    };

    match &widget.kind {
        WidgetKind::Panel { bg, border } => {
            win::fill_rect(dis.hDC, &dis.rcItem, *bg);
            if let Some(border) = border {
                win::frame_rect(dis.hDC, &dis.rcItem, *border);
            }
        }
        WidgetKind::Text {
            text,
            font,
            color,
            bg,
            align,
            multiline,
            vcenter,
        } => {
            win::fill_rect(dis.hDC, &dis.rcItem, *bg);
            let mut rect = dis.rcItem;
            if !*multiline {
                rect.left += 2;
                rect.right -= 2;
            }
            let mut format = DT_NOPREFIX | align.flags();
            if *multiline {
                format |= DT_WORDBREAK;
            } else {
                format |= DT_SINGLELINE;
                if *vcenter {
                    format |= DT_VCENTER;
                }
            }
            win::draw_text(dis.hDC, text, &mut rect, fonts.get(*font), *color, format);
        }
        WidgetKind::Button {
            bg,
            fg,
            font,
            disabled_bg,
            border,
        } => {
            let disabled = (dis.itemState & ODS_DISABLED) != 0;
            let background = if disabled { *disabled_bg } else { *bg };
            win::fill_rect(dis.hDC, &dis.rcItem, background);
            if let Some(border) = border {
                win::frame_rect(dis.hDC, &dis.rcItem, *border);
            }
            let text = win::get_text(hwnd);
            let mut rect = dis.rcItem;
            win::draw_text(
                dis.hDC,
                &text,
                &mut rect,
                fonts.get(*font),
                *fg,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
            );
        }
    }
}

// ---------------------------------------------------------------------------
// 数值输入（EDIT + msctls_updown32）
// ---------------------------------------------------------------------------

pub struct NumberInput {
    pub edit: HWND,
    pub updown: HWND,
    pub max: i32,
    pub increment: i32,
}

impl NumberInput {
    #[allow(clippy::too_many_arguments)]
    pub fn create(
        parent: HWND,
        id: usize,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        max: i32,
        increment: i32,
    ) -> Self {
        let updown_width = ((height as f64) * 0.75).round().max(15.0) as i32;
        let edit_width = (width - updown_width).max(40);
        let edit = win::create_child(
            parent,
            "EDIT",
            "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | (ES_AUTOHSCROLL as u32) | (ES_RIGHT as u32),
            WS_EX_CLIENTEDGE,
            x,
            y,
            edit_width,
            height,
            id,
        );
        let updown = win::create_child(
            parent,
            "msctls_updown32",
            "",
            WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT,
            0,
            x + edit_width,
            y,
            updown_width,
            height,
            id + 5000,
        );
        win::send(updown, UDM_SETBUDDY, edit as usize, 0);
        win::send(updown, UDM_SETRANGE32, 0, max as isize);

        Self {
            edit,
            updown,
            max,
            increment,
        }
    }

    pub fn value(&self) -> i32 {
        let text = win::get_text(self.edit);
        let cleaned: String = text.chars().filter(|c| c.is_ascii_digit()).collect();
        let value: i32 = cleaned.parse().unwrap_or(0);
        value.clamp(0, self.max)
    }

    pub fn set_value(&self, value: i32) {
        let value = value.clamp(0, self.max);
        win::send(self.updown, UDM_SETPOS32, 0, value as isize);
        // UDS_SETBUDDYINT 会把数字写回 buddy；这里兜底一次，避免空值。
        if win::get_text(self.edit).is_empty() {
            win::set_text(self.edit, &format_number(value));
        }
    }

    pub fn clamp_and_set(&self) {
        self.set_value(self.value());
    }

    /// 处理 UDN_DELTAPOS：按各自步进调整并阻止默认的 ±1。
    pub fn handle_delta(&self, delta: i32) -> bool {
        let target = self
            .value()
            .saturating_add(self.increment.saturating_mul(delta));
        self.set_value(target);
        true
    }
}

pub fn format_number(value: i32) -> String {
    let digits = value.abs().to_string();
    let mut grouped = String::new();
    for (index, ch) in digits.chars().rev().enumerate() {
        if index > 0 && index % 3 == 0 {
            grouped.push(',');
        }
        grouped.push(ch);
    }
    let mut result: String = grouped.chars().rev().collect();
    if value < 0 {
        result.insert(0, '-');
    }
    result
}

// ---------------------------------------------------------------------------
// ListView
// ---------------------------------------------------------------------------

pub struct ListView {
    pub hwnd: HWND,
}

impl ListView {
    pub fn create(parent: HWND, id: usize, x: i32, y: i32, width: i32, height: i32) -> Self {
        let hwnd = win::create_child(
            parent,
            "SysListView32",
            "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
            WS_EX_CLIENTEDGE,
            x,
            y,
            width,
            height,
            id,
        );
        win::send(
            hwnd,
            LVM_SETEXTENDEDLISTVIEWSTYLE,
            0,
            (LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_DOUBLEBUFFER)
                as isize,
        );
        Self { hwnd }
    }

    pub fn add_column(&self, text: &str, width: i32) {
        let mut text_buffer = win::wide(text);
        let mut column: LVCOLUMNW = unsafe { std::mem::zeroed() };
        column.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        column.fmt = 0; // LVCFMT_LEFT
        column.cx = width;
        column.pszText = text_buffer.as_mut_ptr();
        column.cchTextMax = text_buffer.len() as i32;
        column.iSubItem = 0;
        win::send(
            self.hwnd,
            LVM_INSERTCOLUMNW,
            0,
            &column as *const LVCOLUMNW as isize,
        );
    }

    pub fn clear(&self) {
        win::send(self.hwnd, LVM_DELETEALLITEMS, 0, 0);
    }

    pub fn insert_row(&self, index: i32, text: &str, checked: bool) {
        let mut text_buffer = win::wide(text);
        let mut item: LVITEMW = unsafe { std::mem::zeroed() };
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = index;
        item.pszText = text_buffer.as_mut_ptr();
        item.cchTextMax = text_buffer.len() as i32;
        item.lParam = index as isize;
        win::send(
            self.hwnd,
            LVM_INSERTITEMW,
            0,
            &item as *const LVITEMW as isize,
        );
        self.set_checked(index, checked);
    }

    pub fn set_checked(&self, index: i32, checked: bool) {
        let mut item: LVITEMW = unsafe { std::mem::zeroed() };
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.state = if checked { 2u32 << 12 } else { 1u32 << 12 };
        win::send(
            self.hwnd,
            LVM_SETITEMSTATE,
            index as usize,
            &item as *const LVITEMW as isize,
        );
    }

    pub fn is_checked(&self, index: i32) -> bool {
        let state = win::send(
            self.hwnd,
            LVM_GETITEMSTATE,
            index as usize,
            LVIS_STATEIMAGEMASK as isize,
        );
        (state as u32 & LVIS_STATEIMAGEMASK) == (2u32 << 12)
    }

    pub fn set_font(&self, font: HFONT) {
        win::set_font(self.hwnd, font);
    }
}
