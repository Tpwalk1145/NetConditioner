//! 配置编辑对话框。

use std::collections::HashSet;

use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::HDC;
use windows_sys::Win32::UI::Controls::*;
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::app::{App, Dialog};
use crate::config::{self, ConditionProtocol, DirectionProfile, NetworkProfile};
use crate::ui::ids;
use crate::ui::widgets::{
    self, Align, FontRole, NumberInput, CARD, DANGER_BG, DANGER_BORDER, DANGER_TEXT, TEXT_BUTTON,
    TEXT_DARK, TEXT_MUTED,
};
use crate::win;

const CLIENT_WIDTH: i32 = 760;
const CLIENT_HEIGHT: i32 = 592;
const EM_SETLIMITTEXT: u32 = 0x00C5;

pub enum EditorOutcome {
    Saved(NetworkProfile),
    Deleted,
}

pub struct ProfileEditor {
    pub hwnd: HWND,
    pub closed: bool,
    existing_id: String,
    existing_names: HashSet<String>,
    name_edit: HWND,
    up: Vec<NumberInput>,
    down: Vec<NumberInput>,
    protocol_combo: HWND,
    outcome: Option<EditorOutcome>,
}

impl ProfileEditor {
    pub fn create(
        app: &mut App,
        existing: Option<&NetworkProfile>,
        allow_delete: bool,
        existing_names: HashSet<String>,
    ) -> Self {
        let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
        let scale = app.main.as_ref().map(|main| main.scale).unwrap_or(1.0);
        let fonts = &app.fonts;
        let s = |value: i32| (value as f64 * scale).round() as i32;

        let title = if existing.is_some() {
            "编辑配置"
        } else {
            "新建配置"
        };
        let hwnd = win::create_window(
            "NCProfileEditor",
            title,
            win::STYLE_DIALOG,
            WS_EX_CONTROLPARENT,
            owner,
            0,
            std::ptr::null_mut(),
        );
        win::set_window_client_size(hwnd, s(CLIENT_WIDTH), s(CLIENT_HEIGHT));
        win::center_on_owner(hwnd, owner);

        let widgets_map = &mut app.widgets;

        widgets::create_panel(
            hwnd,
            ids::EDITOR_HEADER,
            0,
            0,
            s(760),
            s(58),
            widgets::ACCENT,
            None,
            widgets_map,
        );
        widgets::create_text(
            hwnd,
            ids::EDITOR_TITLE,
            title,
            s(20),
            s(14),
            s(400),
            s(30),
            FontRole::Title,
            0x00FF_FFFF,
            widgets::ACCENT,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_text(
            hwnd,
            ids::EDITOR_NAME_LABEL,
            "名称",
            s(20),
            s(80),
            s(44),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        let name_edit = widgets::create_edit(
            hwnd,
            ids::EDITOR_NAME_EDIT,
            "",
            s(70),
            s(76),
            s(670),
            s(27),
            false,
            Align::Left,
            None,
        );
        win::send(name_edit, EM_SETLIMITTEXT, 60, 0);

        let up_group = widgets::create_group(
            hwnd,
            ids::EDITOR_UP_GROUP,
            "上行",
            s(20),
            s(112),
            s(350),
            s(340),
        );
        let down_group = widgets::create_group(
            hwnd,
            ids::EDITOR_DOWN_GROUP,
            "下行",
            s(390),
            s(112),
            s(350),
            s(340),
        );
        win::set_font(up_group, fonts.get(FontRole::Body));
        win::set_font(down_group, fonts.get(FontRole::Body));

        let up_labels = [
            "上行延时 (ms)",
            "上行延时抖动 (ms)",
            "上行带宽 (kbps)",
            "上行随机丢包 (%)",
            "上行连续丢包-放行 (ms)",
            "上行连续丢包-丢包 (ms)",
        ];
        let down_labels = [
            "下行延时 (ms)",
            "下行延时抖动 (ms)",
            "下行带宽 (kbps)",
            "下行随机丢包 (%)",
            "下行连续丢包-放行 (ms)",
            "下行连续丢包-丢包 (ms)",
        ];
        let limits = [
            (60_000, 10),
            (60_000, 10),
            (1_000_000, 10),
            (100, 1),
            (600_000, 10),
            (600_000, 10),
        ];

        let mut up = Vec::with_capacity(6);
        let mut down = Vec::with_capacity(6);
        for row in 0..6 {
            let y = 112 + 28 + (row as i32) * 42;
            widgets::create_text(
                hwnd,
                ids::EDITOR_ROW_LABEL_BASE + row,
                up_labels[row],
                20 + 14,
                y + 4,
                150,
                20,
                FontRole::Body,
                TEXT_BUTTON,
                widgets::SURFACE,
                Align::Left,
                false,
                widgets_map,
            );
            let (max, increment) = limits[row];
            up.push(NumberInput::create(
                hwnd,
                ids::EDITOR_UP_BASE + row,
                s(20 + 170),
                s(y),
                s(165),
                s(24),
                max,
                increment,
            ));
            win::set_font(up[row].edit, fonts.get(FontRole::Body));

            widgets::create_text(
                hwnd,
                ids::EDITOR_ROW_LABEL_BASE + 10 + row,
                down_labels[row],
                390 + 14,
                y + 4,
                150,
                20,
                FontRole::Body,
                TEXT_BUTTON,
                widgets::SURFACE,
                Align::Left,
                false,
                widgets_map,
            );
            down.push(NumberInput::create(
                hwnd,
                ids::EDITOR_DOWN_BASE + row,
                s(390 + 170),
                s(y),
                s(165),
                s(24),
                max,
                increment,
            ));
            win::set_font(down[row].edit, fonts.get(FontRole::Body));
        }

        widgets::create_text(
            hwnd,
            ids::EDITOR_PROTOCOL_LABEL,
            "弱网控制协议",
            s(20),
            s(470),
            s(110),
            s(20),
            FontRole::Body,
            TEXT_DARK,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        let protocol_combo = widgets::create_combo(
            hwnd,
            ids::EDITOR_PROTOCOL_COMBO,
            s(130),
            s(466),
            s(170),
            s(200),
        );
        for item in ["TCP (1)", "UDP (2)", "ALL (3)"] {
            let text = win::wide(item);
            win::send(protocol_combo, CB_ADDSTRING, 0, text.as_ptr() as isize);
        }
        win::send(protocol_combo, CB_SETCURSEL, 2, 0);
        win::set_font(protocol_combo, fonts.get(FontRole::Body));

        widgets::create_text(
            hwnd,
            ids::EDITOR_HINT,
            "带宽单位 kbps；随机丢包单位 %；延时 / 连续丢包单位 ms；0 表示不启用。\r\n连续丢包：放行 X ms、丢包 Y ms 循环；只填“丢包”表示一直丢包。上行丢包/限速也会影响下载 ACK。",
            s(20),
            s(502),
            s(720),
            s(38),
            FontRole::Small,
            TEXT_MUTED,
            widgets::SURFACE,
            Align::Left,
            true,
            widgets_map,
        );

        widgets::create_button(
            hwnd,
            ids::EDITOR_CANCEL,
            "取消",
            s(520),
            s(548),
            s(100),
            s(30),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            widgets::GRAY,
            Some(widgets::BORDER),
            false,
            widgets_map,
        );
        widgets::create_button(
            hwnd,
            ids::EDITOR_OK,
            "确定",
            s(630),
            s(548),
            s(110),
            s(30),
            widgets::ACCENT,
            0x00FF_FFFF,
            FontRole::BodyBold,
            widgets::GRAY,
            None,
            false,
            widgets_map,
        );
        if allow_delete {
            widgets::create_button(
                hwnd,
                ids::EDITOR_DELETE,
                "删除配置",
                s(20),
                s(548),
                s(110),
                s(30),
                DANGER_BG,
                DANGER_TEXT,
                FontRole::Body,
                widgets::GRAY,
                Some(DANGER_BORDER),
                false,
                widgets_map,
            );
        }

        let mut editor = Self {
            hwnd,
            closed: false,
            existing_id: existing
                .map(|profile| profile.id.clone())
                .unwrap_or_default(),
            existing_names,
            name_edit,
            up,
            down,
            protocol_combo,
            outcome: None,
        };
        editor.load_values(existing);
        editor
    }

    fn load_values(&mut self, existing: Option<&NetworkProfile>) {
        match existing {
            Some(profile) => {
                win::set_text(self.name_edit, &profile.name);
                for (index, number) in self.up.iter().enumerate() {
                    number.set_value(match index {
                        0 => profile.uplink.delay_ms,
                        1 => profile.uplink.jitter_ms,
                        2 => profile.uplink.bandwidth_kbps,
                        3 => profile.uplink.random_loss_percent,
                        4 => profile.uplink.burst_allow_ms,
                        _ => profile.uplink.burst_drop_ms,
                    });
                }
                for (index, number) in self.down.iter().enumerate() {
                    number.set_value(match index {
                        0 => profile.downlink.delay_ms,
                        1 => profile.downlink.jitter_ms,
                        2 => profile.downlink.bandwidth_kbps,
                        3 => profile.downlink.random_loss_percent,
                        4 => profile.downlink.burst_allow_ms,
                        _ => profile.downlink.burst_drop_ms,
                    });
                }
                let index = match profile.protocol {
                    ConditionProtocol::Tcp => 0,
                    ConditionProtocol::Udp => 1,
                    ConditionProtocol::All => 2,
                };
                win::send(self.protocol_combo, CB_SETCURSEL, index, 0);
            }
            None => {
                let name = make_default_name(&self.existing_names);
                win::set_text(self.name_edit, &name);
            }
        }
    }

    pub fn on_message(
        &mut self,
        app: &mut App,
        message: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> Option<isize> {
        match message {
            WM_ERASEBKGND => Some(1),
            WM_PAINT => Some(app.paint_surface(self.hwnd)),
            WM_DRAWITEM => Some(app.draw_item(lparam)),
            WM_CTLCOLORSTATIC | WM_CTLCOLOREDIT => {
                win::set_bk_color(wparam as HDC, CARD);
                win::set_text_color(wparam as HDC, TEXT_DARK);
                Some(app.white_brush as isize)
            }
            WM_COMMAND => {
                let id = (wparam & 0xFFFF) as usize;
                let code = ((wparam >> 16) & 0xFFFF) as u32;
                match id {
                    ids::EDITOR_OK => self.confirm(app),
                    ids::EDITOR_CANCEL => self.close(),
                    ids::EDITOR_DELETE if code == BN_CLICKED => self.delete(app),
                    _ => {
                        if code == EN_KILLFOCUS {
                            if let Some(number) = self.number_mut_by_id(id) {
                                number.clamp_and_set();
                            }
                        }
                    }
                }
                Some(0)
            }
            WM_NOTIFY => {
                if lparam != 0 {
                    let header = lparam as *const NMHDR;
                    if unsafe { (*header).code } == UDN_DELTAPOS {
                        let updown = lparam as *const NMUPDOWN;
                        let id = unsafe { (*header).idFrom };
                        let delta = unsafe { (*updown).iDelta };
                        if let Some(number) = self.number_mut_by_updown(id) {
                            number.handle_delta(delta);
                        }
                        return Some(1);
                    }
                }
                Some(0)
            }
            WM_CLOSE => {
                self.close();
                Some(0)
            }
            WM_DESTROY => {
                self.closed = true;
                Some(0)
            }
            _ => None,
        }
    }

    pub fn close(&mut self) {
        self.closed = true;
        win::destroy(self.hwnd);
    }

    fn number_mut_by_id(&mut self, id: usize) -> Option<&mut NumberInput> {
        let up_base = ids::EDITOR_UP_BASE;
        let down_base = ids::EDITOR_DOWN_BASE;
        if id >= up_base && id < up_base + 6 {
            self.up.get_mut(id - up_base)
        } else if id >= down_base && id < down_base + 6 {
            self.down.get_mut(id - down_base)
        } else {
            None
        }
    }

    fn number_mut_by_updown(&mut self, id: usize) -> Option<&mut NumberInput> {
        if id >= 5000 {
            self.number_mut_by_id(id - 5000)
        } else {
            None
        }
    }

    fn read_direction(numbers: &[NumberInput]) -> DirectionProfile {
        DirectionProfile {
            delay_ms: numbers[0].value(),
            jitter_ms: numbers[1].value(),
            bandwidth_kbps: numbers[2].value(),
            random_loss_percent: numbers[3].value(),
            burst_allow_ms: numbers[4].value(),
            burst_drop_ms: numbers[5].value(),
        }
    }

    fn confirm(&mut self, app: &mut App) {
        let name = win::get_text(self.name_edit).trim().to_string();
        if name.is_empty() {
            app.show_message("请输入配置名称。", "配置无效", MB_OK | MB_ICONWARNING);
            win::set_focus(self.name_edit);
            return;
        }

        if self.existing_names.contains(&name.to_lowercase()) {
            app.show_message(
                "已存在同名配置，请换一个名称。",
                "配置无效",
                MB_OK | MB_ICONWARNING,
            );
            win::set_focus(self.name_edit);
            return;
        }

        let protocol = match win::send(self.protocol_combo, CB_GETCURSEL, 0, 0) {
            0 => ConditionProtocol::Tcp,
            1 => ConditionProtocol::Udp,
            _ => ConditionProtocol::All,
        };

        let profile = NetworkProfile {
            id: if self.existing_id.is_empty() {
                config::new_id()
            } else {
                self.existing_id.clone()
            },
            name,
            uplink: Self::read_direction(&self.up),
            downlink: Self::read_direction(&self.down),
            protocol,
        };

        if let Some(error) = profile.validate() {
            app.show_message(&error, "配置无效", MB_OK | MB_ICONWARNING);
            return;
        }

        if profile.is_all_zero() {
            let answer = app.show_message(
                "所有参数均为 0，启动后不会产生弱网效果。仍要保存吗？",
                "配置为空",
                MB_YESNO | MB_ICONQUESTION,
            );
            if answer != IDYES {
                return;
            }
        }

        self.outcome = Some(EditorOutcome::Saved(profile));
        self.close();
    }

    fn delete(&mut self, app: &mut App) {
        let answer = app.show_message(
            "确定删除这个配置吗？",
            "删除配置",
            MB_YESNO | MB_ICONQUESTION,
        );
        if answer != IDYES {
            return;
        }
        self.outcome = Some(EditorOutcome::Deleted);
        self.close();
    }
}

fn make_default_name(existing: &HashSet<String>) -> String {
    let base = "新配置";
    if !existing.contains(base) {
        return base.to_string();
    }
    for index in 2..1000 {
        let candidate = format!("{base} {index}");
        if !existing.contains(&candidate) {
            return candidate;
        }
    }
    format!("{base} {}", &config::new_id()[..6])
}

pub fn open(
    app: &mut App,
    existing: Option<&NetworkProfile>,
    allow_delete: bool,
) -> Option<EditorOutcome> {
    let existing_id = existing
        .map(|profile| profile.id.clone())
        .unwrap_or_default();
    let existing_names: HashSet<String> = app
        .config
        .profiles
        .iter()
        .filter(|profile| profile.id != existing_id)
        .map(|profile| profile.name.to_lowercase())
        .collect();

    let editor = ProfileEditor::create(app, existing, allow_delete, existing_names);
    let hwnd = editor.hwnd;
    let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
    app.dialog = Some(Dialog::Editor(editor));
    win::run_modal(owner, hwnd);

    match app.dialog.take() {
        Some(Dialog::Editor(editor)) => editor.outcome,
        _ => None,
    }
}
