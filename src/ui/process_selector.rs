//! 目标进程选择对话框。

use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::HDC;
use windows_sys::Win32::UI::Controls::*;
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::app::{App, Dialog};
use crate::config::MAX_PROCESS_COUNT;
use crate::engine::process_filter;
use crate::ui::ids;
use crate::ui::widgets::{
    self, Align, FontRole, ListView, CARD, TEXT_BUTTON, TEXT_DARK, TEXT_MUTED,
};
use crate::win;

const CLIENT_WIDTH: i32 = 460;
const CLIENT_HEIGHT: i32 = 520;
const EN_CHANGE: u32 = 0x0300;

#[derive(Clone)]
pub struct Row {
    name: String,
    count: u32,
    path: String,
}

pub struct ProcessSelector {
    pub hwnd: HWND,
    pub closed: bool,
    search_edit: HWND,
    list: ListView,
    empty_label: HWND,
    count_label: HWND,
    rows: Vec<Row>,
    displayed: Vec<usize>,
    selected: Vec<String>,
    suppress: bool,
    pub outcome: Option<Vec<String>>,
}

impl ProcessSelector {
    pub fn create(app: &mut App) -> Self {
        let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
        let scale = app.main.as_ref().map(|main| main.scale).unwrap_or(1.0);
        let fonts = &app.fonts;
        let s = |value: i32| (value as f64 * scale).round() as i32;

        let hwnd = win::create_window(
            "NCProcessSelector",
            "选择目标进程",
            win::STYLE_DIALOG,
            WS_EX_CONTROLPARENT,
            owner,
            0,
            std::ptr::null_mut(),
        );
        win::set_window_client_size(hwnd, s(CLIENT_WIDTH), s(CLIENT_HEIGHT));
        win::center_on_owner(hwnd, owner);

        let initial_selection = app.config.settings.process_names.clone();
        let widgets_map = &mut app.widgets;

        widgets::create_text(
            hwnd,
            ids::SELECTOR_TITLE,
            "选择目标进程",
            s(16),
            s(14),
            s(300),
            s(24),
            FontRole::Hotkey,
            TEXT_DARK,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );
        widgets::create_text(
            hwnd,
            ids::SELECTOR_SUBTITLE,
            "勾选后仅对这些程序生效；不勾选 = 全局（所有进程）",
            s(16),
            s(42),
            s(420),
            s(20),
            FontRole::Small85,
            TEXT_MUTED,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        let search_edit = widgets::create_edit(
            hwnd,
            ids::SELECTOR_SEARCH,
            "",
            s(16),
            s(68),
            s(372),
            s(28),
            false,
            Align::Left,
            Some("搜索进程名…"),
        );
        win::set_font(search_edit, fonts.get(FontRole::Body));

        widgets::create_button(
            hwnd,
            ids::SELECTOR_REFRESH,
            "刷新",
            s(396),
            s(68),
            s(48),
            s(28),
            CARD,
            TEXT_BUTTON,
            FontRole::Body,
            widgets::GRAY,
            Some(widgets::BORDER),
            false,
            widgets_map,
        );

        let list = ListView::create(hwnd, ids::SELECTOR_LIST, s(16), s(108), s(428), s(332));
        list.add_column("进程", s(384));
        list.set_font(fonts.get(FontRole::Body));

        let empty_label = widgets::create_text_full(
            hwnd,
            ids::SELECTOR_EMPTY,
            "没有匹配的进程",
            s(17),
            s(129),
            s(426),
            s(309),
            FontRole::Body,
            TEXT_MUTED,
            CARD,
            Align::Center,
            false,
            true,
            widgets_map,
        );
        win::show(empty_label, SW_HIDE);

        let count_label = widgets::create_text(
            hwnd,
            ids::SELECTOR_COUNT,
            "当前：全局",
            s(16),
            s(452),
            s(200),
            s(20),
            FontRole::Small85,
            TEXT_MUTED,
            widgets::SURFACE,
            Align::Left,
            false,
            widgets_map,
        );

        widgets::create_button(
            hwnd,
            ids::SELECTOR_SELECT_ALL,
            "全选",
            s(326),
            s(449),
            s(40),
            s(24),
            widgets::SURFACE,
            widgets::ACCENT,
            FontRole::Link85,
            widgets::SURFACE,
            None,
            true,
            widgets_map,
        );
        widgets::create_button(
            hwnd,
            ids::SELECTOR_CLEAR,
            "清除",
            s(376),
            s(449),
            s(40),
            s(24),
            widgets::SURFACE,
            widgets::ACCENT,
            FontRole::Link85,
            widgets::SURFACE,
            None,
            true,
            widgets_map,
        );

        widgets::create_button(
            hwnd,
            ids::SELECTOR_CANCEL,
            "取消",
            s(252),
            s(478),
            s(92),
            s(32),
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
            ids::SELECTOR_OK,
            "确定",
            s(352),
            s(478),
            s(92),
            s(32),
            widgets::ACCENT,
            0x00FF_FFFF,
            FontRole::BodyBold,
            widgets::GRAY,
            None,
            false,
            widgets_map,
        );

        let mut selector = Self {
            hwnd,
            closed: false,
            search_edit,
            list,
            empty_label,
            count_label,
            rows: Vec::new(),
            displayed: Vec::new(),
            selected: Vec::new(),
            suppress: false,
            outcome: None,
        };

        for name in initial_selection {
            let display = crate::config::normalize_display_name(&name);
            if !display.is_empty() && !selector.contains(&display) {
                selector.selected.push(display);
            }
        }

        selector.reload(app);
        selector
    }

    fn contains(&self, name: &str) -> bool {
        self.selected
            .iter()
            .any(|existing| existing.eq_ignore_ascii_case(name))
    }

    fn add_selected(&mut self, name: &str) {
        if !self.contains(name) {
            self.selected.push(name.to_string());
        }
    }

    fn remove_selected(&mut self, name: &str) {
        self.selected
            .retain(|existing| !existing.eq_ignore_ascii_case(name));
    }

    pub fn reload(&mut self, app: &mut App) {
        let mut grouped: Vec<Row> = Vec::new();
        for process in process_filter::enumerate_processes() {
            if let Some(row) = grouped
                .iter_mut()
                .find(|row| row.name.eq_ignore_ascii_case(&process.name))
            {
                row.count += 1;
                if row.path.is_empty() && !process.path.is_empty() {
                    row.path = process.path;
                }
            } else {
                grouped.push(Row {
                    name: process.name,
                    count: 1,
                    path: process.path,
                });
            }
        }

        for name in self.selected.clone() {
            if !grouped
                .iter()
                .any(|row| row.name.eq_ignore_ascii_case(&name))
            {
                grouped.push(Row {
                    name,
                    count: 0,
                    path: String::new(),
                });
            }
        }

        self.rows = grouped;
        self.rebuild(app);
    }

    fn rebuild(&mut self, app: &mut App) {
        let filter = win::get_text(self.search_edit).trim().to_lowercase();
        self.suppress = true;
        self.list.clear();
        self.displayed.clear();

        let mut order: Vec<usize> = (0..self.rows.len()).collect();
        order.sort_by(|left, right| {
            let left_row = &self.rows[*left];
            let right_row = &self.rows[*right];
            let left_rank = if self.contains(&left_row.name) {
                0
            } else if left_row.count == 0 {
                2
            } else {
                1
            };
            let right_rank = if self.contains(&right_row.name) {
                0
            } else if right_row.count == 0 {
                2
            } else {
                1
            };
            left_rank.cmp(&right_rank).then_with(|| {
                left_row
                    .name
                    .to_lowercase()
                    .cmp(&right_row.name.to_lowercase())
            })
        });

        let mut list_index = 0i32;
        for row_index in order {
            let row = &self.rows[row_index];
            if !filter.is_empty() && !row.name.to_lowercase().contains(&filter) {
                continue;
            }
            let checked = self.contains(&row.name);
            self.list.insert_row(list_index, &row.name, checked);
            self.displayed.push(row_index);
            list_index += 1;
        }

        self.suppress = false;
        win::show(
            self.empty_label,
            if self.displayed.is_empty() {
                SW_SHOW
            } else {
                SW_HIDE
            },
        );
        self.update_count(app);
    }

    fn update_count(&mut self, app: &mut App) {
        let text = if self.selected.is_empty() {
            "当前：全局".to_string()
        } else {
            format!("已选 {} 个", self.selected.len())
        };
        widgets::set_widget_text(&mut app.widgets, self.count_label, &text);
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
            WM_CTLCOLOREDIT => {
                win::set_bk_color(wparam as HDC, CARD);
                win::set_text_color(wparam as HDC, TEXT_DARK);
                Some(app.white_brush as isize)
            }
            WM_COMMAND => {
                let id = (wparam & 0xFFFF) as usize;
                let code = ((wparam >> 16) & 0xFFFF) as u32;
                match id {
                    ids::SELECTOR_SEARCH if code == EN_CHANGE => self.rebuild(app),
                    ids::SELECTOR_REFRESH => self.reload(app),
                    ids::SELECTOR_SELECT_ALL => self.select_all_visible(app),
                    ids::SELECTOR_CLEAR => self.clear_selection(app),
                    ids::SELECTOR_OK => self.confirm(app),
                    ids::SELECTOR_CANCEL => self.close(),
                    _ => {}
                }
                Some(0)
            }
            WM_NOTIFY => {
                if lparam != 0 {
                    let header = lparam as *const NMHDR;
                    let code = unsafe { (*header).code };
                    if code == LVN_ITEMCHANGED {
                        if !self.suppress {
                            let item = lparam as *const NMLISTVIEW;
                            let index = unsafe { (*item).iItem };
                            let new_state = unsafe { (*item).uNewState };
                            let old_state = unsafe { (*item).uOldState };
                            if index >= 0 && (new_state & 0xF000) != (old_state & 0xF000) {
                                let checked = self.list.is_checked(index);
                                if let Some(row_index) = self.displayed.get(index as usize) {
                                    let name = self.rows[*row_index].name.clone();
                                    if checked {
                                        self.add_selected(&name);
                                    } else {
                                        self.remove_selected(&name);
                                    }
                                    self.update_count(app);
                                }
                            }
                        }
                    } else if code == NM_DBLCLK {
                        let item = lparam as *const NMITEMACTIVATE;
                        let index = unsafe { (*item).iItem };
                        if index >= 0 {
                            let checked = self.list.is_checked(index);
                            self.list.set_checked(index, !checked);
                        }
                    } else if code == LVN_GETINFOTIP {
                        let info = lparam as *mut NMLVGETINFOTIPW;
                        let index = unsafe { (*info).iItem };
                        if index >= 0 {
                            if let Some(row_index) = self.displayed.get(index as usize) {
                                let row = &self.rows[*row_index];
                                let text = if row.count == 0 {
                                    "当前未运行（按进程名匹配，启动后生效）".to_string()
                                } else if row.path.is_empty() {
                                    format!("实例：{}", row.count)
                                } else {
                                    format!("实例：{}\n{}", row.count, row.path)
                                };
                                let size = unsafe { (*info).cchTextMax }.max(1) as usize;
                                let buffer = unsafe {
                                    std::slice::from_raw_parts_mut((*info).pszText, size)
                                };
                                win::write_fixed(buffer, &text);
                            }
                        }
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

    fn select_all_visible(&mut self, app: &mut App) {
        self.suppress = true;
        for (list_index, row_index) in self.displayed.clone().iter().enumerate() {
            let name = self.rows[*row_index].name.clone();
            self.add_selected(&name);
            self.list.set_checked(list_index as i32, true);
        }
        self.suppress = false;
        self.update_count(app);
    }

    fn clear_selection(&mut self, app: &mut App) {
        self.selected.clear();
        self.rebuild(app);
    }

    pub fn close(&mut self) {
        self.closed = true;
        win::destroy(self.hwnd);
    }

    fn confirm(&mut self, app: &mut App) {
        if self.selected.len() > MAX_PROCESS_COUNT {
            app.show_message(
                &format!("最多选择 {MAX_PROCESS_COUNT} 个进程。"),
                "选择目标进程",
                MB_OK | MB_ICONWARNING,
            );
            return;
        }

        let mut names = self.selected.clone();
        names.sort_by_key(|name| name.to_lowercase());
        self.outcome = Some(names);
        self.close();
    }
}

pub fn open(app: &mut App) -> Option<Vec<String>> {
    let selector = ProcessSelector::create(app);
    let hwnd = selector.hwnd;
    let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
    app.dialog = Some(Dialog::Selector(selector));
    win::run_modal(owner, hwnd);

    match app.dialog.take() {
        Some(Dialog::Selector(selector)) => selector.outcome,
        _ => None,
    }
}
