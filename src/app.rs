//! 应用状态与主窗口消息处理。

use std::cell::{Cell, RefCell, RefMut};
use std::collections::HashMap;
use std::sync::atomic::{AtomicIsize, AtomicPtr, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Arc;
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::{HBRUSH, HDC};
use windows_sys::Win32::UI::Controls::DRAWITEMSTRUCT;
use windows_sys::Win32::UI::Shell::{NIIF_ERROR, NIIF_INFO};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::config::{self, AppConfig, NetworkProfile, ProfileStore, MAX_PROCESS_COUNT};
use crate::engine::{EngineEvent, EngineEvents, NetworkConditioner};
use crate::hotkey;
use crate::ui::ids;
use crate::ui::main_window::{self, MainWindow};
use crate::ui::process_selector::ProcessSelector;
use crate::ui::profile_editor::{EditorOutcome, ProfileEditor};
use crate::ui::settings::{SettingsDialog, SettingsOutcome};
use crate::ui::tray::{Tray, TrayCommand};
use crate::ui::widgets::{self, Fonts, Widget, CARD, GRAY, GREEN, ORANGE, RED, SURFACE, TEXT_DARK};
use crate::update::coordinator::{
    self, TickOutcome, UpdateEvent, UpdateSettingsSnapshot, UpdaterState,
};
use crate::update::UpdateService;
use crate::version;
use crate::win;

const TIMER_STATS: usize = 1;
const TIMER_STARTUP_UPDATE: usize = 2;
const TIMER_PERIODIC_UPDATE: usize = 3;
const HOTKEY_ID: i32 = 0x4E43; // 'NC'

static APP_PTR: AtomicPtr<RefCell<App>> = AtomicPtr::new(std::ptr::null_mut());
thread_local! {
    static MODAL_ACTIVE: Cell<bool> = const { Cell::new(false) };
}

pub fn try_app() -> Option<RefMut<'static, App>> {
    let pointer = APP_PTR.load(Ordering::Relaxed);
    if pointer.is_null() {
        return None;
    }
    unsafe { &*pointer }.try_borrow_mut().ok()
}

struct ModalGuard;

impl ModalGuard {
    fn new() -> Option<Self> {
        if MODAL_ACTIVE.with(|active| active.replace(true)) {
            return None;
        }
        Some(Self)
    }
}

impl Drop for ModalGuard {
    fn drop(&mut self) {
        MODAL_ACTIVE.with(|active| active.set(false));
    }
}

#[derive(Clone, Copy)]
enum MainAction {
    CreateProfile,
    EditProfile,
    SelectProcesses,
    ChangeSettings,
}

pub fn run() -> i32 {
    let app = Box::new(RefCell::new(App::new()));
    APP_PTR.store(
        &*app as *const RefCell<App> as *mut RefCell<App>,
        Ordering::Relaxed,
    );
    app.borrow_mut().build();
    app.borrow_mut().after_show();
    let code = win::message_loop();
    drop(app);
    code
}

#[derive(Clone)]
struct Waker {
    hwnd: Arc<AtomicIsize>,
}

impl Waker {
    fn new() -> Self {
        Self {
            hwnd: Arc::new(AtomicIsize::new(0)),
        }
    }

    fn set(&self, hwnd: HWND) {
        self.hwnd.store(hwnd as isize, Ordering::Relaxed);
    }

    fn wake(&self) {
        let value = self.hwnd.load(Ordering::Relaxed);
        if value != 0 {
            win::post(value as HWND, win::WM_APP_EVENTS);
        }
    }
}

pub enum Dialog {
    Editor(ProfileEditor),
    Settings(SettingsDialog),
    Selector(ProcessSelector),
}

impl Dialog {
    pub fn hwnd(&self) -> HWND {
        match self {
            Dialog::Editor(editor) => editor.hwnd,
            Dialog::Settings(settings) => settings.hwnd,
            Dialog::Selector(selector) => selector.hwnd,
        }
    }

    pub fn fonts(&self) -> &Fonts {
        match self {
            Dialog::Editor(editor) => editor.fonts(),
            Dialog::Settings(settings) => settings.fonts(),
            Dialog::Selector(selector) => selector.fonts(),
        }
    }
}

pub struct App {
    pub config: AppConfig,
    pub store: ProfileStore,
    pub load_warning: Option<String>,
    pub engine: Arc<NetworkConditioner>,
    pub engine_events: Arc<EngineEvents>,
    pub engine_rx: Receiver<EngineEvent>,
    pub update_tx: Sender<UpdateEvent>,
    pub update_rx: Receiver<UpdateEvent>,
    waker: Waker,
    pub widgets: HashMap<isize, Widget>,
    pub fonts: Fonts,
    pub main: Option<MainWindow>,
    pub tray: Option<Tray>,
    taskbar_created_message: u32,
    pub hotkey_registered: bool,
    pub hotkey_error: String,
    pub is_running: bool,
    pub is_busy: bool,
    pub exit_requested: bool,
    pub stop_silent: bool,
    pub hide_balloon_shown: bool,
    pub remote_warning_shown: bool,
    pub suppress_profile_change: bool,
    pub run_started_at: Option<Instant>,
    pub auto_stop_at: Option<Instant>,
    pub dialog: Option<Dialog>,
    pending_action: Option<MainAction>,
    pub updater: UpdaterState,
    update_generation: u64,
    active_update_request: Option<(u64, String)>,
    pending_update_request: Option<(u64, String)>,
    pub white_brush: HBRUSH,
}

impl Drop for App {
    fn drop(&mut self) {
        if let Some(hwnd) = self.main_hwnd() {
            win::unregister_hotkey(hwnd, HOTKEY_ID);
        }
        if let Some(mut tray) = self.tray.take() {
            tray.destroy();
        }
        self.fonts.destroy();
        if !self.white_brush.is_null() {
            win::delete_object(self.white_brush);
            self.white_brush = std::ptr::null_mut();
        }
    }
}

impl App {
    fn new() -> Self {
        let store = ProfileStore::with_default_path();
        let loaded = store.load();

        let (engine_tx, engine_rx) = mpsc::channel();
        let waker = Waker::new();
        let wake = {
            let waker = waker.clone();
            Box::new(move || waker.wake())
        };
        let engine_events = Arc::new(EngineEvents::new(engine_tx, wake));
        let engine = Arc::new(NetworkConditioner::new(engine_events.clone()));
        let (update_tx, update_rx) = mpsc::channel();

        let fonts = Fonts::new(win::system_dpi());
        let white_brush = win::create_white_brush();

        Self {
            config: loaded.config,
            store,
            load_warning: loaded.warning,
            engine,
            engine_events,
            engine_rx,
            update_tx,
            update_rx,
            waker,
            widgets: HashMap::new(),
            fonts,
            main: None,
            tray: None,
            taskbar_created_message: 0,
            hotkey_registered: false,
            hotkey_error: String::new(),
            is_running: false,
            is_busy: false,
            exit_requested: false,
            stop_silent: false,
            hide_balloon_shown: false,
            remote_warning_shown: false,
            suppress_profile_change: false,
            run_started_at: None,
            auto_stop_at: None,
            dialog: None,
            pending_action: None,
            updater: UpdaterState::default(),
            update_generation: 0,
            active_update_request: None,
            pending_update_request: None,
            white_brush,
        }
    }

    fn build(&mut self) {
        register_classes();

        let hwnd = MainWindow::create_window();
        let mut main = MainWindow::new(hwnd);
        self.waker.set(hwnd);

        self.fonts.destroy();
        self.fonts = Fonts::new(main.dpi);
        main.build(&mut self.widgets, &self.fonts);
        main.apply_fonts(&self.fonts);
        widgets::apply_fonts(&self.widgets, &self.fonts);
        self.main = Some(main);

        self.tray = Some(Tray::new(hwnd));
        self.taskbar_created_message =
            unsafe { RegisterWindowMessageW(win::wide("TaskbarCreated").as_ptr()) };
        self.register_hotkey(self.config.hotkey.modifiers, self.config.hotkey.key);

        win::set_timer(hwnd, TIMER_STATS, 500);
        win::set_timer(hwnd, TIMER_STARTUP_UPDATE, 5000);
        win::set_timer(hwnd, TIMER_PERIODIC_UPDATE, 6 * 60 * 60 * 1000);

        UpdateService::cleanup_old_files();
        self.bind_profiles();
        self.update_ui();
        self.update_stats();

        win::show(hwnd, SW_SHOW);
        win::set_foreground(hwnd);
    }

    fn after_show(&mut self) {
        if let Some(warning) = self.load_warning.clone() {
            let owner = self.main_hwnd().unwrap_or(std::ptr::null_mut());
            win::message_box(owner, &warning, "配置提示", MB_OK | MB_ICONWARNING);
        }

        if !self.hotkey_registered {
            let owner = self.main_hwnd().unwrap_or(std::ptr::null_mut());
            let message = format!(
                "全局快捷键注册失败：{}\r\n你也可以用窗口按钮启动/停止。",
                self.hotkey_error
            );
            win::message_box(owner, &message, "快捷键", MB_OK | MB_ICONWARNING);
        }
    }

    pub fn main_hwnd(&self) -> Option<HWND> {
        self.main.as_ref().map(|main| main.hwnd)
    }

    pub fn show_message(&self, text: &str, title: &str, flags: u32) -> i32 {
        let owner = self.main_hwnd().unwrap_or(std::ptr::null_mut());
        win::message_box(owner, text, title, flags)
    }

    pub fn cleanup_dialog_widgets(&mut self, hwnd: HWND) {
        self.widgets.remove(&(hwnd as isize));
        for child in win::child_windows(hwnd) {
            self.widgets.remove(&(child as isize));
        }
    }

    pub fn button_colors(&self, hwnd: HWND) -> Option<(u32, u32)> {
        let widget = self.widgets.get(&(hwnd as isize))?;
        match &widget.kind {
            widgets::WidgetKind::Button {
                bg,
                fg,
                disabled_bg,
                ..
            } => Some((
                if win::is_window_enabled(hwnd) {
                    *bg
                } else {
                    *disabled_bg
                },
                *fg,
            )),
            _ => None,
        }
    }

    pub fn draw_item(&self, lparam: LPARAM) -> isize {
        if lparam != 0 {
            let dis = unsafe { &*(lparam as *const DRAWITEMSTRUCT) };
            let parent = win::parent_hwnd(dis.hwndItem);
            let fonts = self
                .dialog
                .as_ref()
                .filter(|dialog| dialog.hwnd() == dis.hwndItem || dialog.hwnd() == parent)
                .map_or(&self.fonts, Dialog::fonts);
            widgets::draw_item(dis, &self.widgets, fonts);
            1
        } else {
            0
        }
    }

    pub fn paint_surface(&self, hwnd: HWND) -> isize {
        let (hdc, mut paint) = win::create_paint_dc(hwnd);
        let rect = win::client_rect(hwnd);
        win::fill_rect(hdc, &rect, SURFACE);
        win::end_paint(hwnd, &mut paint);
        0
    }

    pub fn save_config(&mut self) {
        if let Err(error) = self.store.save(&self.config) {
            self.show_message(
                &format!("保存配置失败：{error}"),
                "NetConditioner",
                MB_OK | MB_ICONERROR,
            );
        }
    }

    pub fn show_balloon(&mut self, title: &str, text: &str, flags: u32) {
        if let Some(tray) = &mut self.tray {
            tray.balloon(title, text, flags);
        }
    }

    fn register_hotkey(&mut self, modifiers: u32, key: i32) {
        let Some(hwnd) = self.main_hwnd() else {
            return;
        };
        win::unregister_hotkey(hwnd, HOTKEY_ID);

        if !hotkey::is_valid(modifiers, key) {
            self.hotkey_registered = false;
            self.hotkey_error =
                "请指定一个非修饰键；可以直接用单个键（如 F8），也可以加 Ctrl/Alt/Shift/Win。"
                    .to_string();
            return;
        }

        if win::register_hotkey(hwnd, HOTKEY_ID, modifiers, key) {
            self.hotkey_registered = true;
            self.hotkey_error.clear();
            return;
        }

        let code = win::last_error_code();
        self.hotkey_registered = false;
        self.hotkey_error = if code == 1409 {
            "该快捷键已被其他程序占用，请换一个组合。".to_string()
        } else {
            format!("注册快捷键失败（Win32 错误 {code}）。")
        };
    }

    fn bind_profiles(&mut self) {
        let Some(controls) = self.main.as_ref().map(|main| main.controls) else {
            return;
        };

        self.suppress_profile_change = true;
        win::send(controls.profile_combo, CB_RESETCONTENT, 0, 0);
        for profile in &self.config.profiles {
            let name = win::wide(&profile.name);
            win::send(
                controls.profile_combo,
                CB_ADDSTRING,
                0,
                name.as_ptr() as isize,
            );
        }

        if let Some(profile) = self.config.find_active_profile() {
            let index = self
                .config
                .profiles
                .iter()
                .position(|candidate| candidate.id == profile.id)
                .unwrap_or(0);
            win::send(controls.profile_combo, CB_SETCURSEL, index, 0);
            self.config.active_profile_id = Some(profile.id.clone());
        }
        self.suppress_profile_change = false;
    }

    fn update_stats(&mut self) {
        let Some(controls) = self.main.as_ref().map(|main| main.controls) else {
            return;
        };

        if self.is_running {
            let uplink = self.engine.uplink_stats();
            let downlink = self.engine.downlink_stats();
            let up_text = match uplink {
                Some(stats) => format!(
                    "上行   发送 {}   丢弃 {}   队列 {}",
                    stats.sent, stats.dropped, stats.queue
                ),
                None => "上行   发送 0   丢弃 0   队列 0".to_string(),
            };
            let down_text = match downlink {
                Some(stats) => format!(
                    "下行   发送 {}   丢弃 {}   队列 {}",
                    stats.sent, stats.dropped, stats.queue
                ),
                None => "下行   发送 0   丢弃 0   队列 0".to_string(),
            };
            widgets::set_widget_text(&mut self.widgets, controls.uplink_stats, &up_text);
            widgets::set_widget_text(&mut self.widgets, controls.downlink_stats, &down_text);

            if let Some(auto_stop_at) = self.auto_stop_at {
                if Instant::now() >= auto_stop_at {
                    self.auto_stop_at = None;
                    self.stop_engine_sync();
                    self.update_ui();
                    self.show_balloon("已自动恢复网络", "已达到最长运行时间", NIIF_INFO);
                    self.update_stats();
                }
            }
        } else {
            widgets::set_widget_text(
                &mut self.widgets,
                controls.uplink_stats,
                "上行   发送 0   丢弃 0   队列 0",
            );
            widgets::set_widget_text(
                &mut self.widgets,
                controls.downlink_stats,
                "下行   发送 0   丢弃 0   队列 0",
            );
        }
    }

    fn update_ui(&mut self) {
        let Some(controls) = self.main.as_ref().map(|main| main.controls) else {
            return;
        };

        let (toggle_text, toggle_bg, toggle_enabled) = if self.is_busy {
            ("正在应用…", GRAY, false)
        } else if self.is_running {
            ("停止并恢复", RED, true)
        } else {
            ("启动弱网", GREEN, true)
        };
        win::set_text(controls.toggle_button, toggle_text);
        widgets::set_widget_button_colors(
            &mut self.widgets,
            controls.toggle_button,
            toggle_bg,
            0x00FF_FFFF,
        );
        win::enable(controls.toggle_button, toggle_enabled);

        let (dot_color, status, detail) = if self.is_busy {
            (ORANGE, "正在切换…".to_string(), "请稍候".to_string())
        } else if self.is_running {
            let name = self
                .config
                .find_active_profile()
                .map(|profile| profile.name.clone())
                .unwrap_or_else(|| "配置".to_string());
            (
                GREEN,
                format!("已连接：{name}"),
                format!(
                    "弱网参数生效：{}",
                    format_process_scope(&self.config.settings.process_names)
                ),
            )
        } else {
            (
                GRAY,
                "未连接".to_string(),
                "网络正常，未应用任何弱网参数".to_string(),
            )
        };
        widgets::set_widget_text_color(&mut self.widgets, controls.status_dot, dot_color);
        widgets::set_widget_text(&mut self.widgets, controls.status_label, &status);
        widgets::set_widget_text(&mut self.widgets, controls.status_detail, &detail);

        let enabled_idle = !self.is_running && !self.is_busy;
        win::enable(controls.profile_combo, enabled_idle);
        win::enable(controls.new_button, enabled_idle);
        win::enable(controls.edit_button, !self.is_busy);
        win::enable(controls.delete_button, enabled_idle);
        win::enable(controls.process_button, enabled_idle);
        win::enable(controls.settings_link, !self.is_busy);

        let scope = format_process_names(&self.config.settings.process_names);
        win::set_text(controls.process_box, &scope);

        let hotkey_text = if self.hotkey_registered {
            hotkey::format(self.config.hotkey.modifiers, self.config.hotkey.key)
        } else {
            "未注册（点击设置）".to_string()
        };
        widgets::set_widget_text(&mut self.widgets, controls.hotkey_label, &hotkey_text);

        if let Some(tray) = &mut self.tray {
            tray.set_running(self.is_running);
        }
    }

    fn schedule_auto_stop(&mut self) {
        let minutes = self.config.settings.max_run_minutes.max(0);
        self.auto_stop_at = if minutes > 0 {
            self.run_started_at
                .map(|started| started + Duration::from_secs(minutes as u64 * 60))
        } else {
            None
        };
    }

    fn stop_engine_sync(&mut self) {
        self.engine.stop();
        self.is_running = false;
        self.is_busy = false;
        self.auto_stop_at = None;
    }

    fn toggle(&mut self) {
        if self.is_busy {
            return;
        }
        if self.is_running {
            self.stop_async(false);
        } else {
            self.start_async();
        }
    }

    fn start_async(&mut self) {
        if self.is_busy || self.is_running {
            return;
        }

        let Some(profile) = self.config.find_active_profile().cloned() else {
            return;
        };

        if let Some(error) = profile.validate() {
            self.show_message(&error, "配置无效", MB_OK | MB_ICONWARNING);
            return;
        }

        if profile.is_all_zero() {
            let answer = self.show_message(
                "当前配置所有参数均为 0，启动后不会产生弱网效果。仍要启动吗？",
                "配置为空",
                MB_YESNO | MB_ICONQUESTION,
            );
            if answer != IDYES {
                return;
            }
        }

        if win::is_remote_session() && !self.remote_warning_shown {
            self.remote_warning_shown = true;
            let answer = self.show_message(
                "当前是远程桌面会话。开启弱网（尤其是高丢包/低带宽）可能导致远程连接卡顿或断开。\r\n\r\n建议先设置“最长运行时间”，确保断线后能自动恢复。仍要启动吗？",
                "远程会话提醒",
                MB_YESNO | MB_ICONWARNING,
            );
            if answer != IDYES {
                return;
            }
        }

        let process_names = self.config.settings.process_names.clone();
        let max_queue = self.config.settings.max_queue_delay_ms;
        let engine = self.engine.clone();
        let events = self.engine_events.clone();

        self.is_busy = true;
        self.update_ui();

        std::thread::spawn(move || {
            let result = engine.start(&profile, None, None, max_queue, &process_names);
            events.send(EngineEvent::StartDone(result));
        });
    }

    fn stop_async(&mut self, silent: bool) {
        if !self.is_running || self.is_busy {
            return;
        }

        self.is_busy = true;
        self.stop_silent = silent;
        self.update_ui();

        let engine = self.engine.clone();
        let events = self.engine_events.clone();
        std::thread::spawn(move || {
            engine.stop();
            events.send(EngineEvent::StopDone);
        });
    }

    fn on_start_done(&mut self, result: Result<(), String>) {
        match result {
            Ok(()) => {
                self.is_running = true;
                self.is_busy = false;
                self.run_started_at = Some(Instant::now());
                self.schedule_auto_stop();
                self.engine.reset_stats();
                self.update_ui();
                self.update_stats();
                let name = self
                    .config
                    .find_active_profile()
                    .map(|profile| profile.name.clone())
                    .unwrap_or_default();
                self.show_balloon("弱网模拟已启动", &format!("配置：{name}"), NIIF_INFO);
            }
            Err(error) => {
                self.engine.stop();
                self.is_running = false;
                self.is_busy = false;
                self.update_ui();
                self.update_stats();
                self.show_message(
                    &format!("启动弱网模拟失败：\r\n{error}"),
                    "NetConditioner",
                    MB_OK | MB_ICONERROR,
                );
            }
        }
    }

    fn on_stop_done(&mut self) {
        let silent = self.stop_silent;
        self.is_running = false;
        self.is_busy = false;
        self.auto_stop_at = None;
        self.update_ui();
        self.update_stats();
        if !silent {
            self.show_balloon("已恢复网络", "弱网模拟已停止", NIIF_INFO);
        }
        self.maybe_tick_update();
    }

    fn on_engine_fault(&mut self, message: String) {
        if !self.is_running {
            return;
        }
        self.stop_engine_sync();
        self.update_ui();
        self.update_stats();
        self.show_balloon("弱网模拟已停止", "引擎错误，网络已恢复", NIIF_ERROR);
        self.show_message(&message, "弱网模拟已停止", MB_OK | MB_ICONERROR);
    }

    fn drain_events(&mut self) {
        loop {
            match self.engine_rx.try_recv() {
                Ok(EngineEvent::StartDone(result)) => self.on_start_done(result),
                Ok(EngineEvent::StopDone) => self.on_stop_done(),
                Ok(EngineEvent::Fault(message)) => self.on_engine_fault(message),
                Err(_) => break,
            }
        }

        loop {
            match self.update_rx.try_recv() {
                Ok(UpdateEvent::Tick(outcome)) => {
                    self.updater.busy = false;
                    let request = self.active_update_request.take();
                    let current = (
                        self.update_generation,
                        self.config.settings.update_feed_url.trim().to_string(),
                    );
                    let auto_update = self.config.settings.auto_update;
                    if request.as_ref() != Some(&current) || !auto_update {
                        UpdateService::log("忽略来自旧更新设置的检查结果。");
                        self.maybe_tick_update();
                        continue;
                    }
                    if matches!(&outcome, TickOutcome::Ready { .. }) {
                        self.pending_update_request = request.clone();
                    }
                    self.apply_update_outcome(outcome);
                }
                Err(_) => break,
            }
        }
    }

    fn maybe_tick_update(&mut self) {
        if self.updater.install_started {
            return;
        }
        if self.updater.pending.is_some() {
            self.try_install_pending();
            return;
        }
        if self.updater.busy {
            return;
        }

        let settings = &self.config.settings;
        if !settings.auto_update || settings.update_feed_url.trim().is_empty() {
            return;
        }

        let now = config::now_unix();
        if !coordinator::should_check(settings, now) {
            return;
        }

        self.updater.busy = true;
        let feed_url = settings.update_feed_url.trim().to_string();
        self.active_update_request = Some((self.update_generation, feed_url.clone()));
        let snapshot = UpdateSettingsSnapshot {
            last_update_attempt_unix: settings.last_update_attempt_unix,
            last_attempted_version: settings.last_attempted_version.clone(),
        };
        let sender = self.update_tx.clone();
        let waker = self.waker.clone();
        coordinator::spawn_tick(
            feed_url,
            version::current(),
            snapshot,
            sender,
            Box::new(move || waker.wake()),
        );
    }

    fn apply_update_outcome(&mut self, outcome: TickOutcome) {
        match outcome {
            TickOutcome::CheckFailed { error } => {
                UpdateService::log(&format!("检查更新失败：{error}"));
            }
            TickOutcome::UpToDate { now } => {
                self.config.settings.last_update_check_unix = now;
                self.save_config();
                UpdateService::log("已是最新版本。");
            }
            TickOutcome::Ready {
                manifest,
                installer,
                now,
            } => {
                self.config.settings.last_update_check_unix = now;
                self.updater.pending = Some((manifest, installer));
                self.save_config();
                self.try_install_pending();
            }
            TickOutcome::DownloadFailed { manifest, now } => {
                self.config.settings.last_update_check_unix = now;
                self.config.settings.last_update_attempt_unix = now;
                self.config.settings.last_attempted_version =
                    manifest.version.clone().unwrap_or_default();
                self.save_config();
            }
        }
    }

    fn is_idle(&self) -> bool {
        !self.is_running && !self.is_busy && self.dialog.is_none()
    }

    fn try_install_pending(&mut self) {
        if self.updater.install_started {
            return;
        }
        let Some((manifest, installer)) = self.updater.pending.clone() else {
            return;
        };
        let current = (
            self.update_generation,
            self.config.settings.update_feed_url.trim().to_string(),
        );
        if !self.config.settings.auto_update
            || self.config.settings.update_feed_url.trim().is_empty()
            || self.pending_update_request.as_ref() != Some(&current)
        {
            self.updater.pending = None;
            self.pending_update_request = None;
            return;
        }

        if !self.is_idle() {
            if !self.updater.deferred_logged {
                self.updater.deferred_logged = true;
                UpdateService::log("当前弱网运行中或有窗口打开，暂不安装，等空闲后再继续。");
            }
            return;
        }

        self.updater.install_started = true;
        self.updater.deferred_logged = false;
        UpdateService::log(&format!(
            "开始自更新到版本 {}。",
            manifest.version.clone().unwrap_or_default()
        ));

        self.stop_engine_sync();

        let attempted_at = config::now_unix();
        self.config.settings.last_update_attempt_unix = attempted_at;
        self.config.settings.last_attempted_version = manifest.version.clone().unwrap_or_default();
        self.save_config();

        if !UpdateService::apply_self_update(&installer) {
            self.updater.install_started = false;
            UpdateService::log("启动自更新失败，继续使用当前版本。");
            return;
        }

        if UpdateService::is_dry_run() {
            UpdateService::log("DRY RUN：保持当前进程运行，不重启。");
        } else {
            self.exit_for_update();
        }
    }

    fn exit_for_update(&mut self) {
        self.exit_requested = true;
        if let Some(hwnd) = self.main_hwnd() {
            win::kill_timer(hwnd, TIMER_STATS);
            win::kill_timer(hwnd, TIMER_STARTUP_UPDATE);
            win::kill_timer(hwnd, TIMER_PERIODIC_UPDATE);
        }
        self.stop_engine_sync();
        if let Some(hwnd) = self.main_hwnd() {
            win::destroy(hwnd);
        }
    }

    fn delete_profile(&mut self) {
        if self.is_running || self.is_busy {
            return;
        }
        let Some(current) = self.config.find_active_profile().cloned() else {
            return;
        };

        let answer = self.show_message(
            &format!("确定删除配置“{}”吗？", current.name),
            "删除配置",
            MB_YESNO | MB_ICONQUESTION,
        );
        if answer == IDYES {
            self.delete_profile_by_id(&current.id);
        }
    }

    fn delete_profile_by_id(&mut self, id: &str) {
        if self.is_running || self.is_busy {
            return;
        }
        let Some(index) = self
            .config
            .profiles
            .iter()
            .position(|profile| profile.id == id)
        else {
            return;
        };

        self.config.profiles.remove(index);
        if self.config.profiles.is_empty() {
            let fallback = NetworkProfile::default();
            self.config.active_profile_id = Some(fallback.id.clone());
            self.config.profiles.push(fallback);
        } else if self.config.active_profile_id.as_deref() == Some(id) {
            self.config.active_profile_id = Some(self.config.profiles[0].id.clone());
        }

        self.save_config();
        self.bind_profiles();
        self.update_ui();
    }

    fn hide_to_tray(&mut self) {
        if let Some(hwnd) = self.main_hwnd() {
            win::show(hwnd, SW_HIDE);
        }
        if !self.hide_balloon_shown {
            self.hide_balloon_shown = true;
            self.show_balloon(
                "仍在后台运行",
                "点击托盘图标可重新打开；退出请使用托盘菜单。",
                NIIF_INFO,
            );
        }
        self.maybe_tick_update();
    }

    fn show_from_tray(&mut self) {
        if let Some(hwnd) = self.main_hwnd() {
            win::show(hwnd, SW_SHOW);
            win::set_foreground(hwnd);
        }
    }

    fn exit_app(&mut self) {
        self.exit_requested = true;
        if let Some(hwnd) = self.main_hwnd() {
            win::kill_timer(hwnd, TIMER_STATS);
            win::kill_timer(hwnd, TIMER_STARTUP_UPDATE);
            win::kill_timer(hwnd, TIMER_PERIODIC_UPDATE);
        }
        self.stop_engine_sync();
        if let Some(hwnd) = self.main_hwnd() {
            win::destroy(hwnd);
        }
    }

    fn on_profile_changed(&mut self) {
        if self.suppress_profile_change {
            return;
        }
        let Some(controls) = self.main.as_ref().map(|main| main.controls) else {
            return;
        };
        let index = win::send(controls.profile_combo, CB_GETCURSEL, 0, 0);
        if index < 0 {
            return;
        }
        if let Some(profile) = self.config.profiles.get(index as usize) {
            self.config.active_profile_id = Some(profile.id.clone());
            self.save_config();
        }
    }

    fn on_tray_command(&mut self, command: TrayCommand) {
        match command {
            TrayCommand::Show => self.show_from_tray(),
            TrayCommand::Toggle => self.toggle(),
            TrayCommand::Settings => self.pending_action = Some(MainAction::ChangeSettings),
            TrayCommand::Exit => self.exit_app(),
        }
    }

    fn on_dialog_message(
        &mut self,
        hwnd: HWND,
        message: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> Option<isize> {
        let mut dialog = self.dialog.take()?;
        if dialog.hwnd() != hwnd {
            self.dialog = Some(dialog);
            return None;
        }

        let result = match &mut dialog {
            Dialog::Editor(editor) => editor.on_message(self, message, wparam, lparam),
            Dialog::Settings(settings) => settings.on_message(self, message, wparam, lparam),
            Dialog::Selector(selector) => selector.on_message(self, message, wparam, lparam),
        };
        self.dialog = Some(dialog);
        result
    }

    fn on_main_message(
        &mut self,
        hwnd: HWND,
        message: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> Option<isize> {
        match message {
            WM_ERASEBKGND => Some(1),
            WM_PAINT => Some(self.paint_surface(hwnd)),
            WM_DRAWITEM => Some(self.draw_item(lparam)),
            WM_CTLCOLORSTATIC => {
                win::set_bk_color(wparam as HDC, CARD);
                win::set_text_color(wparam as HDC, TEXT_DARK);
                Some(self.white_brush as isize)
            }
            WM_CTLCOLORBTN => {
                let hwnd = lparam as HWND;
                let colors = self.button_colors(hwnd);
                colors.map(|(background, foreground)| {
                    win::set_dc_brush_color(wparam as HDC, background);
                    win::set_text_color(wparam as HDC, foreground);
                    win::dc_brush() as isize
                })
            }
            WM_COMMAND => {
                let id = wparam & 0xFFFF;
                let code = ((wparam >> 16) & 0xFFFF) as u32;
                match id {
                    ids::MAIN_PROFILE_COMBO if code == CBN_SELCHANGE => self.on_profile_changed(),
                    ids::MAIN_NEW => self.pending_action = Some(MainAction::CreateProfile),
                    ids::MAIN_EDIT => self.pending_action = Some(MainAction::EditProfile),
                    ids::MAIN_DELETE => self.delete_profile(),
                    ids::MAIN_PROCESS_BUTTON => {
                        self.pending_action = Some(MainAction::SelectProcesses)
                    }
                    ids::MAIN_TOGGLE => self.toggle(),
                    ids::MAIN_SETTINGS => self.pending_action = Some(MainAction::ChangeSettings),
                    ids::MAIN_MINIMIZE => self.hide_to_tray(),
                    ids::MAIN_EXIT => self.exit_app(),
                    _ => {}
                }
                Some(0)
            }
            WM_TIMER => {
                match wparam {
                    TIMER_STATS => self.update_stats(),
                    TIMER_STARTUP_UPDATE => {
                        win::kill_timer(hwnd, TIMER_STARTUP_UPDATE);
                        self.maybe_tick_update();
                    }
                    TIMER_PERIODIC_UPDATE => self.maybe_tick_update(),
                    _ => {}
                }
                Some(0)
            }
            WM_HOTKEY => {
                if wparam as i32 == HOTKEY_ID {
                    self.toggle();
                }
                Some(0)
            }
            win::WM_APP_EVENTS => {
                self.drain_events();
                Some(0)
            }
            win::WM_APP_TRAY => {
                let command = self
                    .tray
                    .as_mut()
                    .and_then(|tray| tray.handle_message(lparam, self.is_running));
                if let Some(command) = command {
                    self.on_tray_command(command);
                }
                Some(0)
            }
            _ if self.taskbar_created_message != 0 && message == self.taskbar_created_message => {
                if let Some(tray) = &mut self.tray {
                    tray.restore();
                }
                Some(0)
            }
            WM_CLOSE => {
                if self.exit_requested {
                    win::destroy(hwnd);
                } else {
                    self.hide_to_tray();
                }
                Some(0)
            }
            WM_DPICHANGED => {
                self.on_dpi_changed(wparam, lparam);
                Some(0)
            }
            WM_DESTROY => {
                self.on_destroy(hwnd);
                Some(0)
            }
            _ => None,
        }
    }

    fn on_dpi_changed(&mut self, wparam: WPARAM, lparam: LPARAM) {
        let new_dpi = (wparam as u32) & 0xFFFF;
        if new_dpi == 0 {
            return;
        }

        let Some(mut main) = self.main.take() else {
            return;
        };

        if let Some(dialog) = self.dialog.take() {
            self.cleanup_dialog_widgets(dialog.hwnd());
            win::destroy(dialog.hwnd());
        }

        // 销毁旧子控件后按新 DPI 重建。
        unsafe {
            let mut child = GetWindow(main.hwnd, GW_CHILD);
            while !child.is_null() {
                let next = GetWindow(child, GW_HWNDNEXT);
                win::destroy(child);
                child = next;
            }
        }
        self.widgets.clear();

        self.fonts.destroy();
        main.scale = win::fit_dialog_scale(
            main.hwnd,
            main_window::CLIENT_WIDTH,
            main_window::CLIENT_HEIGHT,
            new_dpi as f64 / 96.0,
        );
        main.dpi = (main.scale * 96.0).round() as u32;
        self.fonts = Fonts::new(main.dpi);
        main.build(&mut self.widgets, &self.fonts);
        main.apply_fonts(&self.fonts);
        widgets::apply_fonts(&self.widgets, &self.fonts);

        if lparam != 0 {
            let rect = unsafe { &*(lparam as *const RECT) };
            win::set_window_pos(
                main.hwnd,
                rect.left,
                rect.top,
                rect.right - rect.left,
                rect.bottom - rect.top,
            );
        }

        self.main = Some(main);
        self.bind_profiles();
        self.update_ui();
        self.update_stats();
    }

    fn on_destroy(&mut self, hwnd: HWND) {
        win::kill_timer(hwnd, TIMER_STATS);
        win::kill_timer(hwnd, TIMER_STARTUP_UPDATE);
        win::kill_timer(hwnd, TIMER_PERIODIC_UPDATE);

        if let Some(mut tray) = self.tray.take() {
            tray.destroy();
        }
        win::unregister_hotkey(hwnd, HOTKEY_ID);
        self.hotkey_registered = false;

        self.stop_engine_sync();

        self.fonts.destroy();
        win::delete_object(self.white_brush);
        self.white_brush = std::ptr::null_mut();

        unsafe {
            PostQuitMessage(0);
        }
    }

    pub fn settings_hotkey_pressed(&mut self, modifiers: u32, key: i32) {
        if let Some(Dialog::Settings(settings)) = &mut self.dialog {
            settings.set_candidate(modifiers, key, &mut self.widgets, &self.fonts);
        }
    }

    pub fn settings_request_cancel(&mut self) {
        let Some(Dialog::Settings(mut settings)) = self.dialog.take() else {
            return;
        };
        settings.cancel(self);
        self.dialog = Some(Dialog::Settings(settings));
    }

    pub fn settings_request_confirm(&mut self) {
        let Some(mut dialog) = self.dialog.take() else {
            return;
        };
        if let Dialog::Settings(settings) = &mut dialog {
            settings.confirm(self);
        }
        self.dialog = Some(dialog);
    }
}

fn execute_main_action(action: MainAction) {
    match action {
        MainAction::CreateProfile => execute_create_profile(),
        MainAction::EditProfile => execute_edit_profile(),
        MainAction::SelectProcesses => execute_select_processes(),
        MainAction::ChangeSettings => execute_change_settings(),
    }
}

fn run_profile_editor(
    existing: Option<NetworkProfile>,
    allow_delete: bool,
) -> Option<EditorOutcome> {
    let (hwnd, owner) = {
        let mut app = try_app()?;
        if app.dialog.is_some() || app.is_busy || (existing.is_none() && app.is_running) {
            return None;
        }
        let existing_id = existing
            .as_ref()
            .map(|profile| profile.id.clone())
            .unwrap_or_default();
        let existing_names = app
            .config
            .profiles
            .iter()
            .filter(|profile| profile.id != existing_id)
            .map(|profile| profile.name.to_lowercase())
            .collect();
        let editor =
            ProfileEditor::create(&mut app, existing.as_ref(), allow_delete, existing_names);
        let hwnd = editor.hwnd;
        let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
        app.dialog = Some(Dialog::Editor(editor));
        (hwnd, owner)
    };

    let _guard = ModalGuard::new()?;
    win::run_modal(owner, hwnd);

    let mut app = try_app()?;
    match app.dialog.take() {
        Some(Dialog::Editor(editor)) => editor.outcome,
        _ => None,
    }
}

fn execute_create_profile() {
    let Some(EditorOutcome::Saved(profile)) = run_profile_editor(None, false) else {
        return;
    };
    let Some(mut app) = try_app() else {
        return;
    };
    if app.is_running || app.is_busy {
        return;
    }
    app.config.active_profile_id = Some(profile.id.clone());
    app.config.profiles.push(profile);
    app.save_config();
    app.bind_profiles();
    app.update_ui();
}

fn execute_edit_profile() {
    let Some((current, was_running)) = try_app().and_then(|app| {
        if app.is_busy || app.dialog.is_some() {
            return None;
        }
        app.config
            .find_active_profile()
            .cloned()
            .map(|profile| (profile, app.is_running))
    }) else {
        return;
    };

    let Some(outcome) = run_profile_editor(Some(current.clone()), !was_running) else {
        if let Some(mut app) = try_app() {
            app.update_ui();
        }
        return;
    };

    let Some(mut app) = try_app() else {
        return;
    };
    match outcome {
        EditorOutcome::Deleted => app.delete_profile_by_id(&current.id),
        EditorOutcome::Saved(profile) => {
            let Some(index) = app
                .config
                .profiles
                .iter()
                .position(|candidate| candidate.id == current.id)
            else {
                return;
            };
            app.config.profiles[index] = profile.clone();
            app.config.active_profile_id = Some(profile.id.clone());
            app.save_config();
            app.bind_profiles();
            app.update_ui();

            if was_running {
                app.stop_engine_sync();
                app.update_ui();
                app.start_async();
            }
        }
    }
}

fn run_process_selector() -> Option<Vec<String>> {
    let (hwnd, owner) = {
        let mut app = try_app()?;
        if app.dialog.is_some() || app.is_running || app.is_busy {
            return None;
        }
        let selector = ProcessSelector::create(&mut app);
        let hwnd = selector.hwnd;
        let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
        app.dialog = Some(Dialog::Selector(selector));
        (hwnd, owner)
    };

    let _guard = ModalGuard::new()?;
    win::run_modal(owner, hwnd);

    let mut app = try_app()?;
    match app.dialog.take() {
        Some(Dialog::Selector(selector)) => selector.outcome,
        _ => None,
    }
}

fn execute_select_processes() {
    let Some(names) = run_process_selector() else {
        return;
    };
    let Some(mut app) = try_app() else {
        return;
    };
    if names.len() > MAX_PROCESS_COUNT {
        app.show_message(
            &format!("目标进程最多 {MAX_PROCESS_COUNT} 个。"),
            "目标进程",
            MB_OK | MB_ICONWARNING,
        );
        app.update_ui();
        return;
    }
    app.config.settings.process_names = names;
    app.save_config();
    app.update_ui();
}

fn run_settings_dialog() -> Option<(SettingsOutcome, config::HotkeySettings)> {
    let (hwnd, owner, old_hotkey) = {
        let mut app = try_app()?;
        if app.dialog.is_some() || app.is_busy {
            return None;
        }
        let old_hotkey = app.config.hotkey.clone();
        if let Some(hwnd) = app.main_hwnd() {
            win::unregister_hotkey(hwnd, HOTKEY_ID);
        }
        app.hotkey_registered = false;
        let dialog = SettingsDialog::create(&mut app);
        let hwnd = dialog.hwnd;
        let owner = app.main_hwnd().unwrap_or(std::ptr::null_mut());
        app.dialog = Some(Dialog::Settings(dialog));
        (hwnd, owner, old_hotkey)
    };

    let _guard = ModalGuard::new()?;
    win::run_modal(owner, hwnd);

    let mut app = try_app()?;
    let outcome = match app.dialog.take() {
        Some(Dialog::Settings(dialog)) => dialog.outcome,
        _ => None,
    };
    outcome.map(|outcome| (outcome, old_hotkey))
}

fn execute_change_settings() {
    let Some((outcome, old_hotkey)) = run_settings_dialog() else {
        if let Some(mut app) = try_app() {
            if !app.hotkey_registered {
                let hotkey = app.config.hotkey.clone();
                app.register_hotkey(hotkey.modifiers, hotkey.key);
            }
            app.update_ui();
        }
        return;
    };

    let SettingsOutcome {
        hotkey: new_hotkey,
        max_run_minutes,
        max_queue_delay_ms,
        auto_update,
        update_feed_url,
    } = outcome;

    let Some(mut app) = try_app() else {
        return;
    };
    app.register_hotkey(new_hotkey.modifiers, new_hotkey.key);
    if !app.hotkey_registered {
        let new_error = app.hotkey_error.clone();
        app.register_hotkey(old_hotkey.modifiers, old_hotkey.key);
        let message = format!(
            "新的快捷键无法注册：{new_error}\r\n{}",
            if app.hotkey_registered {
                "已保留原快捷键。"
            } else {
                "原快捷键也无法重新注册，请尽快重新设置。"
            }
        );
        app.show_message(&message, "快捷键", MB_OK | MB_ICONWARNING);
        app.update_ui();
        return;
    }

    let update_settings_changed = auto_update != app.config.settings.auto_update
        || update_feed_url.trim() != app.config.settings.update_feed_url.trim();
    if update_settings_changed {
        app.update_generation = app.update_generation.wrapping_add(1);
        app.active_update_request = None;
        app.pending_update_request = None;
        app.updater.pending = None;
    }

    app.config.hotkey = new_hotkey;
    app.config.settings.max_run_minutes = max_run_minutes;
    app.config.settings.max_queue_delay_ms = max_queue_delay_ms;
    app.config.settings.auto_update = auto_update;
    app.config.settings.update_feed_url = update_feed_url;
    if app.is_running {
        app.schedule_auto_stop();
    }
    app.save_config();
    app.update_ui();
    app.maybe_tick_update();
}

fn format_process_scope(names: &[String]) -> String {
    if names.is_empty() {
        return "全部进程（全局）".to_string();
    }
    if names.len() == 1 {
        return names[0].clone();
    }
    if names.len() <= 3 {
        return names.join("、");
    }
    format!("{} 等 {} 个进程", names[0], names.len())
}

fn format_process_names(names: &[String]) -> String {
    if names.is_empty() {
        "全部进程（全局）".to_string()
    } else {
        names.join(", ")
    }
}

fn register_classes() {
    win::register_class("NCMain", Some(main_proc));
    win::register_class("NCProfileEditor", Some(editor_proc));
    win::register_class("NCSettings", Some(settings_proc));
    win::register_class("NCProcessSelector", Some(selector_proc));
}

unsafe fn defer_main_message(hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match message {
        win::WM_APP_EVENTS | WM_PAINT | WM_TIMER => {
            PostMessageW(hwnd, message, wparam, lparam);
            0
        }
        WM_DESTROY => {
            PostQuitMessage(0);
            0
        }
        _ => DefWindowProcW(hwnd, message, wparam, lparam),
    }
}

unsafe extern "system" fn main_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if MODAL_ACTIVE.with(|active| active.get()) {
        return defer_main_message(hwnd, message, wparam, lparam);
    }
    let Some(mut app) = try_app() else {
        return defer_main_message(hwnd, message, wparam, lparam);
    };
    let result = match app.on_main_message(hwnd, message, wparam, lparam) {
        Some(result) => result,
        None => DefWindowProcW(hwnd, message, wparam, lparam),
    };
    let action = app.pending_action.take();
    drop(app);
    if let Some(action) = action {
        execute_main_action(action);
    }
    result
}

unsafe extern "system" fn editor_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    dispatch_dialog(hwnd, message, wparam, lparam)
}

unsafe extern "system" fn settings_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    dispatch_dialog(hwnd, message, wparam, lparam)
}

unsafe extern "system" fn selector_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    dispatch_dialog(hwnd, message, wparam, lparam)
}

unsafe fn dispatch_dialog(hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    let Some(mut app) = try_app() else {
        return match message {
            WM_PAINT => {
                PostMessageW(hwnd, message, wparam, lparam);
                0
            }
            _ => DefWindowProcW(hwnd, message, wparam, lparam),
        };
    };
    match app.on_dialog_message(hwnd, message, wparam, lparam) {
        Some(result) => result,
        None => DefWindowProcW(hwnd, message, wparam, lparam),
    }
}

#[cfg(test)]
mod tests {
    use super::{ModalGuard, MODAL_ACTIVE};

    #[test]
    fn modal_guard_rejects_reentry_and_resets_on_drop() {
        let first = ModalGuard::new().expect("first guard");
        assert!(MODAL_ACTIVE.with(|active| active.get()));
        assert!(ModalGuard::new().is_none());
        drop(first);
        assert!(!MODAL_ACTIVE.with(|active| active.get()));
        assert!(ModalGuard::new().is_some());
    }
}
