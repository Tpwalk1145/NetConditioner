#include "app.h"

#include <commctrl.h>
#include <process.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/engine.h"
#include "hotkey.h"
#include "store.h"
#include "update/coordinator.h"
#include "update/update.h"
#include "ui/ids.h"
#include "ui/main_window.h"
#include "ui/process_selector.h"
#include "ui/profile_editor.h"
#include "ui/settings.h"
#include "ui/tray.h"
#include "version.h"
#include "win.h"

#define NC_TIMER_STATS 1
#define NC_TIMER_STARTUP_UPDATE 2
#define NC_TIMER_PERIODIC_UPDATE 3
#define NC_HOTKEY_ID 0x4E43

typedef enum {
    NC_DIALOG_NONE = 0,
    NC_DIALOG_EDITOR,
    NC_DIALOG_SETTINGS,
    NC_DIALOG_SELECTOR
} NcDialogKind;

typedef enum {
    NC_ACTION_NONE = 0,
    NC_ACTION_CREATE_PROFILE,
    NC_ACTION_EDIT_PROFILE,
    NC_ACTION_SELECT_PROCESSES,
    NC_ACTION_CHANGE_SETTINGS
} NcMainAction;

typedef struct NcUpdateNotice {
    NcTickOutcome outcome;
    struct NcUpdateNotice *next;
} NcUpdateNotice;

typedef struct NcUpdateBus {
    volatile LONG refs;
    SRWLOCK lock;
    HWND hwnd;
    NcUpdateNotice *head;
    NcUpdateNotice *tail;
} NcUpdateBus;

struct NcApp {
    NcConfig config;
    NcStore store;
    char *load_warning;
    NcNetworkConditioner *engine;
    NcEngineEvents *events;
    NcWidgets widgets;
    NcFonts fonts;
    NcMainWindow main;
    bool main_ready;
    NcTray tray;
    bool tray_ready;
    UINT taskbar_message;
    bool hotkey_registered;
    char hotkey_error[256];
    bool is_running;
    bool is_busy;
    bool exit_requested;
    bool stop_silent;
    bool hide_balloon_shown;
    bool remote_warning_shown;
    bool suppress_profile_change;
    uint64_t run_started_ns;
    bool has_run_started;
    uint64_t auto_stop_ns;
    bool has_auto_stop;
    NcDialogKind dialog_kind;
    void *dialog;
    NcMainAction pending_action;
    HBRUSH white_brush;
    NcUpdateBus *update_bus;
    CRITICAL_SECTION worker_lock;
    CONDITION_VARIABLE worker_cv;
    int worker_count;
    bool update_busy;
    bool update_install_started;
    bool update_deferred_logged;
    bool update_has_pending;
    NcUpdateManifest update_pending_manifest;
    wchar_t update_pending_installer[32768];
    uint64_t update_generation;
    bool update_has_active;
    uint64_t update_active_generation;
    char update_active_feed[2048];
    bool update_has_pending_request;
    uint64_t update_pending_request_generation;
    char update_pending_request_feed[2048];
};

static NcApp *g_app = NULL;
static bool g_modal_active = false;
static bool g_deferred_events = false;

static void update_ui(NcApp *app);
static void update_stats(NcApp *app);
static void save_config(NcApp *app);
static void start_async(NcApp *app);
static void stop_async(NcApp *app, bool silent);
static void stop_engine_sync(NcApp *app);
static void schedule_auto_stop(NcApp *app);
static void bind_profiles(NcApp *app);
static void register_hotkey(NcApp *app, uint32_t modifiers, int32_t key);
static void on_engine_fault(NcApp *app, const char *message);
static void execute_main_action(NcApp *app, NcMainAction action);
static void maybe_tick_update(NcApp *app);
static void try_install_pending(NcApp *app);
static void drain_update_events(NcApp *app);
static void exit_for_update(NcApp *app);

static NcUpdateBus *update_bus_new(HWND hwnd) {
    NcUpdateBus *bus = (NcUpdateBus *)calloc(1, sizeof(NcUpdateBus));
    if (!bus) {
        return NULL;
    }
    InitializeSRWLock(&bus->lock);
    bus->refs = 1;
    bus->hwnd = hwnd;
    return bus;
}

static void update_bus_retain(NcUpdateBus *bus) {
    if (bus) {
        InterlockedIncrement(&bus->refs);
    }
}

static void update_bus_release(NcUpdateBus *bus) {
    NcUpdateNotice *node;
    if (!bus) {
        return;
    }
    if (InterlockedDecrement(&bus->refs) > 0) {
        return;
    }
    AcquireSRWLockExclusive(&bus->lock);
    node = bus->head;
    bus->head = NULL;
    bus->tail = NULL;
    ReleaseSRWLockExclusive(&bus->lock);
    while (node) {
        NcUpdateNotice *next = node->next;
        nc_tick_outcome_free(&node->outcome);
        free(node);
        node = next;
    }
    free(bus);
}

static void update_bus_push(NcUpdateBus *bus, NcTickOutcome *outcome) {
    NcUpdateNotice *node;
    HWND hwnd;
    if (!bus || !outcome) {
        return;
    }
    node = (NcUpdateNotice *)malloc(sizeof(NcUpdateNotice));
    if (!node) {
        nc_tick_outcome_free(outcome);
        return;
    }
    node->outcome = *outcome;
    memset(outcome, 0, sizeof(*outcome));
    node->next = NULL;
    AcquireSRWLockExclusive(&bus->lock);
    if (bus->tail) {
        bus->tail->next = node;
    } else {
        bus->head = node;
    }
    bus->tail = node;
    hwnd = bus->hwnd;
    ReleaseSRWLockExclusive(&bus->lock);
    if (hwnd) {
        PostMessageW(hwnd, NC_WM_APP_EVENTS, 0, 0);
    }
}

static bool update_bus_pop(NcUpdateBus *bus, NcTickOutcome *outcome) {
    NcUpdateNotice *node;
    if (!bus || !outcome) {
        return false;
    }
    AcquireSRWLockExclusive(&bus->lock);
    node = bus->head;
    if (node) {
        bus->head = node->next;
        if (!bus->head) {
            bus->tail = NULL;
        }
    }
    ReleaseSRWLockExclusive(&bus->lock);
    if (!node) {
        return false;
    }
    *outcome = node->outcome;
    free(node);
    return true;
}

NcWidgets *nc_app_widgets(NcApp *app) {
    return app ? &app->widgets : NULL;
}

NcFonts *nc_app_fonts(NcApp *app) {
    return app ? &app->fonts : NULL;
}

HWND nc_app_main_hwnd(NcApp *app) {
    return (app && app->main_ready) ? app->main.hwnd : NULL;
}

double nc_app_scale(NcApp *app) {
    if (!app) {
        return 1.0;
    }
    return app->main_ready ? app->main.scale : 1.0;
}

HBRUSH nc_app_white_brush(NcApp *app) {
    return app ? app->white_brush : NULL;
}

NcConfig *nc_app_config(NcApp *app) {
    return app ? &app->config : NULL;
}

int nc_app_show_message(NcApp *app, const char *text, const char *title, UINT flags) {
    HWND owner = nc_app_main_hwnd(app);
    return nc_message_box(owner, text, title, flags);
}

void nc_app_cleanup_dialog_widgets(NcApp *app, HWND hwnd) {
    if (app) {
        nc_widgets_remove_window(&app->widgets, hwnd);
    }
}

static const NcFonts *dialog_fonts(NcApp *app, HWND item) {
    HWND root;
    if (!app || app->dialog_kind == NC_DIALOG_NONE) {
        return &app->fonts;
    }
    root = GetAncestor(item, GA_ROOT);
    switch (app->dialog_kind) {
    case NC_DIALOG_EDITOR: {
        HWND hwnd = nc_profile_editor_hwnd((NcProfileEditor *)app->dialog);
        if (item == hwnd || root == hwnd) {
            return nc_profile_editor_fonts((NcProfileEditor *)app->dialog);
        }
        break;
    }
    case NC_DIALOG_SETTINGS: {
        HWND hwnd = nc_settings_dialog_hwnd((NcSettingsDialog *)app->dialog);
        if (item == hwnd || root == hwnd) {
            return nc_settings_dialog_fonts((NcSettingsDialog *)app->dialog);
        }
        break;
    }
    case NC_DIALOG_SELECTOR: {
        HWND hwnd = nc_process_selector_hwnd((NcProcessSelector *)app->dialog);
        if (item == hwnd || root == hwnd) {
            return nc_process_selector_fonts((NcProcessSelector *)app->dialog);
        }
        break;
    }
    default:
        break;
    }
    return &app->fonts;
}

LRESULT nc_app_draw_item(NcApp *app, LPARAM lparam) {
    if (app && lparam) {
        DRAWITEMSTRUCT *draw = (DRAWITEMSTRUCT *)lparam;
        nc_widget_draw_item(draw, &app->widgets, dialog_fonts(app, draw->hwndItem));
        return 1;
    }
    return 0;
}

LRESULT nc_app_paint_surface([[maybe_unused]] NcApp *app, HWND hwnd) {
    PAINTSTRUCT paint;
    HDC hdc;
    RECT rect;
    hdc = nc_begin_paint(hwnd, &paint);
    rect = nc_client_rect(hwnd);
    nc_fill_rect(hdc, &rect, NC_COLOR_SURFACE);
    nc_end_paint(hwnd, &paint);
    return 0;
}

int nc_app_button_colors(NcApp *app, HWND hwnd, uint32_t *background, uint32_t *foreground) {
    NcWidget *widget;
    if (!app || !hwnd) {
        return 0;
    }
    widget = nc_widget_find(&app->widgets, hwnd);
    if (!widget || widget->kind != NC_WIDGET_BUTTON) {
        return 0;
    }
    if (background) {
        *background = nc_is_window_enabled(hwnd) ? widget->button_bg : widget->disabled_bg;
    }
    if (foreground) {
        *foreground = widget->button_fg;
    }
    return 1;
}

static void show_balloon(NcApp *app, const char *title, const char *text, UINT flags) {
    if (app && app->tray_ready) {
        nc_tray_balloon(&app->tray, title, text, flags);
    }
}

void nc_app_save_config(NcApp *app) {
    NcErr error;
    if (!app) {
        return;
    }
    if (!nc_store_save(&app->store, &app->config, &error)) {
        char message[1200];
        snprintf(message, sizeof(message), "保存配置失败：%s", error.msg);
        nc_app_show_message(app, message, "NetConditioner", MB_OK | MB_ICONERROR);
    }
}

static void save_config(NcApp *app) {
    nc_app_save_config(app);
}

static void format_process_scope(const NcStringList *names, Str *out) {
    str_clear(out);
    if (!names || names->count == 0) {
        str_add(out, "全部进程（全局）");
        return;
    }
    if (names->count == 1) {
        str_add(out, names->items[0]);
        return;
    }
    if (names->count <= 3) {
        size_t index;
        for (index = 0; index < names->count; index++) {
            if (index > 0) {
                str_add(out, "、");
            }
            str_add(out, names->items[index]);
        }
        return;
    }
    str_addf(out, "%s 等 %u 个进程", names->items[0], (unsigned)names->count);
}

static void format_process_names(const NcStringList *names, Str *out) {
    size_t index;
    str_clear(out);
    if (!names || names->count == 0) {
        str_add(out, "全部进程（全局）");
        return;
    }
    for (index = 0; index < names->count; index++) {
        if (index > 0) {
            str_add(out, ", ");
        }
        str_add(out, names->items[index]);
    }
}

static void register_hotkey(NcApp *app, uint32_t modifiers, int32_t key) {
    HWND hwnd = nc_app_main_hwnd(app);
    if (!hwnd) {
        return;
    }
    nc_unregister_hotkey(hwnd, NC_HOTKEY_ID);
    if (!nc_hotkey_is_valid(modifiers, key)) {
        app->hotkey_registered = false;
        snprintf(app->hotkey_error, sizeof(app->hotkey_error),
                 "请指定一个非修饰键；可以直接用单个键（如 F8），也可以加 Ctrl/Alt/Shift/Win。");
        return;
    }
    if (nc_register_hotkey(hwnd, NC_HOTKEY_ID, modifiers, key)) {
        app->hotkey_registered = true;
        app->hotkey_error[0] = 0;
        return;
    }
    app->hotkey_registered = false;
    if (nc_last_error_code() == 1409) {
        snprintf(app->hotkey_error, sizeof(app->hotkey_error),
                 "该快捷键已被其他程序占用，请换一个组合。");
    } else {
        snprintf(app->hotkey_error, sizeof(app->hotkey_error), "注册快捷键失败（Win32 错误 %d）。",
                 nc_last_error_code());
    }
}

static void bind_profiles(NcApp *app) {
    HWND combo;
    size_t index;
    int selection = 0;
    if (!app || !app->main_ready) {
        return;
    }
    combo = app->main.controls.profile_combo;
    app->suppress_profile_change = true;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (index = 0; index < app->config.profile_count; index++) {
        wchar_t *wide = nc_wide_dup(app->config.profiles[index].name ? app->config.profiles[index].name : "");
        if (wide) {
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)wide);
            free(wide);
        }
    }
    {
        NcProfile *active = nc_config_active_profile(&app->config);
        if (active) {
            size_t found = nc_config_find_profile_index(&app->config, active->id);
            selection = found == (size_t)-1 ? 0 : (int)found;
            free(app->config.active_profile_id);
            app->config.active_profile_id = nc_strdup(active->id ? active->id : "");
        }
    }
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)selection, 0);
    app->suppress_profile_change = false;
}

static void update_stats(NcApp *app) {
    NcMainControls *controls;
    if (!app || !app->main_ready) {
        return;
    }
    controls = &app->main.controls;
    if (app->is_running) {
        NcStatsSnapshot uplink;
        NcStatsSnapshot downlink;
        Str up_text;
        Str down_text;
        memset(&uplink, 0, sizeof(uplink));
        memset(&downlink, 0, sizeof(downlink));
        nc_conditioner_uplink_stats(app->engine, &uplink);
        nc_conditioner_downlink_stats(app->engine, &downlink);
        str_init(&up_text);
        str_init(&down_text);
        str_addf(&up_text, "上行   发送 %llu   丢弃 %llu   队列 %u",
                 (unsigned long long)uplink.sent, (unsigned long long)uplink.dropped,
                 (unsigned)uplink.queue);
        str_addf(&down_text, "下行   发送 %llu   丢弃 %llu   队列 %u",
                 (unsigned long long)downlink.sent, (unsigned long long)downlink.dropped,
                 (unsigned)downlink.queue);
        nc_widget_set_text(&app->widgets, controls->uplink_stats, str_cstr(&up_text));
        nc_widget_set_text(&app->widgets, controls->downlink_stats, str_cstr(&down_text));
        str_free(&up_text);
        str_free(&down_text);
        if (app->has_auto_stop && nc_monotonic_ns() >= app->auto_stop_ns) {
            app->has_auto_stop = false;
            stop_engine_sync(app);
            update_ui(app);
            show_balloon(app, "已自动恢复网络", "已达到最长运行时间", NIIF_INFO);
            update_stats(app);
        }
    } else {
        nc_widget_set_text(&app->widgets, controls->uplink_stats,
                           "上行   发送 0   丢弃 0   队列 0");
        nc_widget_set_text(&app->widgets, controls->downlink_stats,
                           "下行   发送 0   丢弃 0   队列 0");
    }
}

static void update_ui(NcApp *app) {
    NcMainControls *controls;
    const char *toggle_text;
    uint32_t toggle_color;
    bool toggle_enabled;
    if (!app || !app->main_ready) {
        return;
    }
    controls = &app->main.controls;
    if (app->is_busy) {
        toggle_text = "正在应用…";
        toggle_color = NC_COLOR_GRAY;
        toggle_enabled = false;
    } else if (app->is_running) {
        toggle_text = "停止并恢复";
        toggle_color = NC_COLOR_RED;
        toggle_enabled = true;
    } else {
        toggle_text = "启动弱网";
        toggle_color = NC_COLOR_GREEN;
        toggle_enabled = true;
    }
    nc_set_text(controls->toggle_button, toggle_text);
    nc_widget_set_button_colors(&app->widgets, controls->toggle_button, toggle_color, 0x00FFFFFF);
    nc_enable(controls->toggle_button, toggle_enabled);
    if (app->is_busy) {
        nc_widget_set_text_color(&app->widgets, controls->status_dot, NC_COLOR_ORANGE);
        nc_widget_set_text(&app->widgets, controls->status_label, "正在切换…");
        nc_widget_set_text(&app->widgets, controls->status_detail, "请稍候");
    } else if (app->is_running) {
        NcProfile *profile = nc_config_active_profile(&app->config);
        Str label;
        Str detail;
        Str scope;
        str_init(&label);
        str_init(&detail);
        str_init(&scope);
        str_addf(&label, "已连接：%s", profile && profile->name ? profile->name : "配置");
        format_process_scope(&app->config.settings.process_names, &scope);
        str_addf(&detail, "弱网参数生效：%s", str_cstr(&scope));
        nc_widget_set_text_color(&app->widgets, controls->status_dot, NC_COLOR_GREEN);
        nc_widget_set_text(&app->widgets, controls->status_label, str_cstr(&label));
        nc_widget_set_text(&app->widgets, controls->status_detail, str_cstr(&detail));
        str_free(&label);
        str_free(&detail);
        str_free(&scope);
    } else {
        nc_widget_set_text_color(&app->widgets, controls->status_dot, NC_COLOR_GRAY);
        nc_widget_set_text(&app->widgets, controls->status_label, "未连接");
        nc_widget_set_text(&app->widgets, controls->status_detail, "网络正常，未应用任何弱网参数");
    }
    {
        bool enabled_idle = !app->is_running && !app->is_busy;
        nc_enable(controls->profile_combo, enabled_idle);
        nc_enable(controls->new_button, enabled_idle);
        nc_enable(controls->edit_button, !app->is_busy);
        nc_enable(controls->delete_button, enabled_idle);
        nc_enable(controls->process_button, enabled_idle);
        nc_enable(controls->settings_link, !app->is_busy);
    }
    {
        Str names;
        str_init(&names);
        format_process_names(&app->config.settings.process_names, &names);
        nc_set_text(controls->process_box, str_cstr(&names));
        str_free(&names);
    }
    {
        char text[64];
        if (app->hotkey_registered) {
            nc_hotkey_format(app->config.hotkey.modifiers, app->config.hotkey.key, text,
                             sizeof(text));
        } else {
            snprintf(text, sizeof(text), "未注册（点击设置）");
        }
        nc_widget_set_text(&app->widgets, controls->hotkey_label, text);
    }
    if (app->tray_ready) {
        nc_tray_set_running(&app->tray, app->is_running);
    }
}

static void schedule_auto_stop(NcApp *app) {
    int32_t minutes;
    if (!app) {
        return;
    }
    minutes = app->config.settings.max_run_minutes;
    if (minutes < 0) {
        minutes = 0;
    }
    if (minutes > 0 && app->has_run_started) {
        app->auto_stop_ns = app->run_started_ns + (uint64_t)minutes * 60ull * 1000000000ull;
        app->has_auto_stop = true;
    } else {
        app->has_auto_stop = false;
    }
}

static void stop_engine_sync(NcApp *app) {
    if (!app) {
        return;
    }
    nc_conditioner_stop(app->engine);
    app->is_running = false;
    app->is_busy = false;
    app->has_auto_stop = false;
}

static void on_start_done(NcApp *app, bool ok, const char *message) {
    if (ok) {
        NcProfile *profile;
        Str name;
        app->is_running = true;
        app->is_busy = false;
        app->run_started_ns = nc_monotonic_ns();
        app->has_run_started = true;
        schedule_auto_stop(app);
        nc_conditioner_reset_stats(app->engine);
        update_ui(app);
        update_stats(app);
        profile = nc_config_active_profile(&app->config);
        str_init(&name);
        str_add(&name, profile && profile->name ? profile->name : "");
        {
            Str text;
            str_init(&text);
            str_addf(&text, "配置：%s", str_cstr(&name));
            show_balloon(app, "弱网模拟已启动", str_cstr(&text), NIIF_INFO);
            str_free(&text);
        }
        str_free(&name);
    } else {
        Str text;
        nc_conditioner_stop(app->engine);
        app->is_running = false;
        app->is_busy = false;
        update_ui(app);
        update_stats(app);
        str_init(&text);
        str_addf(&text, "启动弱网模拟失败：\r\n%s", message ? message : "");
        nc_app_show_message(app, str_cstr(&text), "NetConditioner", MB_OK | MB_ICONERROR);
        str_free(&text);
    }
}

static void on_stop_done(NcApp *app) {
    bool silent = app->stop_silent;
    app->is_running = false;
    app->is_busy = false;
    app->has_auto_stop = false;
    app->stop_silent = false;
    update_ui(app);
    update_stats(app);
    if (!silent) {
        show_balloon(app, "已恢复网络", "弱网模拟已停止", NIIF_INFO);
    }
    maybe_tick_update(app);
}

static void on_engine_fault(NcApp *app, const char *message) {
    if (!app->is_running) {
        return;
    }
    stop_engine_sync(app);
    update_ui(app);
    update_stats(app);
    show_balloon(app, "弱网模拟已停止", "引擎错误，网络已恢复", NIIF_ERROR);
    nc_app_show_message(app, message ? message : "弱网模拟已停止", "弱网模拟已停止",
                        MB_OK | MB_ICONERROR);
}

typedef struct {
    NcApp *app;
    NcProfile profile;
    char **names;
    size_t name_count;
    int32_t max_queue_delay_ms;
} StartContext;

static void worker_begin(NcApp *app) {
    EnterCriticalSection(&app->worker_lock);
    app->worker_count++;
    LeaveCriticalSection(&app->worker_lock);
}

static void worker_finish(NcApp *app) {
    EnterCriticalSection(&app->worker_lock);
    app->worker_count--;
    if (app->worker_count == 0) {
        WakeAllConditionVariable(&app->worker_cv);
    }
    LeaveCriticalSection(&app->worker_lock);
}

static unsigned __stdcall update_tick_worker(void *parameter);

typedef struct {
    NcUpdateBus *bus;
    char feed[2048];
    NcVersion current;
    NcUpdateSettingsSnapshot snapshot;
} NcUpdateTickContext;

static unsigned __stdcall update_tick_worker(void *parameter) {
    NcUpdateTickContext *context = (NcUpdateTickContext *)parameter;
    NcTickOutcome outcome;
    NcUpdateBus *bus = context->bus;
    nc_coordinator_tick(context->feed, &context->current, &context->snapshot, &outcome);
    update_bus_push(bus, &outcome);
    free(context->snapshot.last_attempted_version);
    update_bus_release(bus);
    free(context);
    return 0;
}

static unsigned __stdcall start_worker(void *parameter) {
    StartContext *context = (StartContext *)parameter;
    NcApp *app = context->app;
    NcErr error;
    NcEngineEvent event;
    bool ok;
    memset(&event, 0, sizeof(event));
    event.kind = NC_ENGINE_EVENT_START_DONE;
    ok = nc_conditioner_start(app->engine, &context->profile, NULL, NULL,
                              context->max_queue_delay_ms,
                              (const char *const *)context->names, context->name_count, &error);
    event.ok = ok;
    if (!ok) {
        snprintf(event.message, sizeof(event.message), "%s", error.msg);
    }
    nc_engine_events_send(app->events, &event);
    nc_config_free_profile(&context->profile);
    {
        size_t index;
        for (index = 0; index < context->name_count; index++) {
            free(context->names[index]);
        }
    }
    free(context->names);
    free(context);
    worker_finish(app);
    return 0;
}

static unsigned __stdcall stop_worker(void *parameter) {
    NcApp *app = (NcApp *)parameter;
    NcEngineEvent event;
    nc_conditioner_stop(app->engine);
    memset(&event, 0, sizeof(event));
    event.kind = NC_ENGINE_EVENT_STOP_DONE;
    event.ok = true;
    nc_engine_events_send(app->events, &event);
    worker_finish(app);
    return 0;
}

static void start_async(NcApp *app) {
    NcProfile *source;
    StartContext *context;
    size_t index;
    HWND thread;
    if (!app || app->is_busy || app->is_running) {
        return;
    }
    source = nc_config_active_profile(&app->config);
    if (!source) {
        return;
    }
    {
        NcErr error;
        if (!nc_profile_validate(source, &error)) {
            nc_app_show_message(app, error.msg, "配置无效", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    if (nc_profile_all_zero(source)) {
        int answer = nc_app_show_message(app,
                                         "当前配置所有参数均为 0，启动后不会产生弱网效果。仍要启动吗？",
                                         "配置为空", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            return;
        }
    }
    if (nc_is_remote_session() && !app->remote_warning_shown) {
        int answer;
        app->remote_warning_shown = true;
        answer = nc_app_show_message(
            app,
            "当前是远程桌面会话。开启弱网（尤其是高丢包/低带宽）可能导致远程连接卡顿或断开。\r\n\r\n"
            "建议先设置“最长运行时间”，确保断线后能自动恢复。仍要启动吗？",
            "远程会话提醒", MB_YESNO | MB_ICONWARNING);
        if (answer != IDYES) {
            return;
        }
    }
    context = (StartContext *)calloc(1, sizeof(StartContext));
    if (!context) {
        return;
    }
    context->app = app;
    if (!nc_config_copy_profile(source, &context->profile)) {
        free(context);
        return;
    }
    context->max_queue_delay_ms = app->config.settings.max_queue_delay_ms;
    context->name_count = app->config.settings.process_names.count;
    if (context->name_count > 0) {
        context->names = (char **)calloc(context->name_count, sizeof(char *));
        if (!context->names) {
            nc_config_free_profile(&context->profile);
            free(context);
            return;
        }
        for (index = 0; index < context->name_count; index++) {
            context->names[index] = nc_strdup(app->config.settings.process_names.items[index]);
        }
    }
    app->is_busy = true;
    update_ui(app);
    worker_begin(app);
    thread = (HWND)_beginthreadex(NULL, 0, start_worker, context, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        worker_finish(app);
        nc_config_free_profile(&context->profile);
        free(context->names);
        free(context);
        app->is_busy = false;
        update_ui(app);
    }
}

static void stop_async(NcApp *app, bool silent) {
    HWND thread;
    if (!app || !app->is_running || app->is_busy) {
        return;
    }
    app->is_busy = true;
    app->stop_silent = silent;
    update_ui(app);
    worker_begin(app);
    thread = (HWND)_beginthreadex(NULL, 0, stop_worker, app, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        worker_finish(app);
        app->is_busy = false;
        update_ui(app);
    }
}

static void drain_events(NcApp *app) {
    NcEngineEvent event;
    while (nc_engine_events_pop(app->events, &event)) {
        switch (event.kind) {
        case NC_ENGINE_EVENT_START_DONE:
            on_start_done(app, event.ok, event.message);
            break;
        case NC_ENGINE_EVENT_STOP_DONE:
            on_stop_done(app);
            break;
        case NC_ENGINE_EVENT_FAULT:
            on_engine_fault(app, event.message);
            break;
        default:
            break;
        }
    }
}

static bool app_is_idle(NcApp *app) {
    return app && !app->is_running && !app->is_busy && !app->dialog;
}

static void maybe_tick_update(NcApp *app) {
    NcUpdateTickContext *context;
    HWND thread;
    NcSettings *settings;
    if (!app || !app->update_bus) {
        return;
    }
    if (app->update_install_started) {
        return;
    }
    if (app->update_has_pending) {
        try_install_pending(app);
        return;
    }
    if (app->update_busy) {
        return;
    }
    settings = &app->config.settings;
    if (!settings->auto_update || !settings->update_feed_url || !settings->update_feed_url[0]) {
        return;
    }
    if (!nc_should_check(settings, nc_now_unix())) {
        return;
    }
    context = (NcUpdateTickContext *)calloc(1, sizeof(NcUpdateTickContext));
    if (!context) {
        return;
    }
    context->bus = app->update_bus;
    update_bus_retain(app->update_bus);
    snprintf(context->feed, sizeof(context->feed), "%s", settings->update_feed_url);
    context->current = nc_version_current();
    context->snapshot.last_update_attempt_unix = settings->last_update_attempt_unix;
    context->snapshot.last_attempted_version =
        nc_strdup(settings->last_attempted_version ? settings->last_attempted_version : "");
    app->update_busy = true;
    app->update_has_active = true;
    app->update_active_generation = app->update_generation;
    snprintf(app->update_active_feed, sizeof(app->update_active_feed), "%s", context->feed);
    thread = (HWND)_beginthreadex(NULL, 0, update_tick_worker, context, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        update_bus_release(app->update_bus);
        free(context->snapshot.last_attempted_version);
        free(context);
        app->update_busy = false;
        app->update_has_active = false;
    }
}

static void exit_for_update(NcApp *app) {
    app->exit_requested = true;
    if (app->main_ready) {
        nc_kill_timer(app->main.hwnd, NC_TIMER_STATS);
        nc_kill_timer(app->main.hwnd, NC_TIMER_STARTUP_UPDATE);
        nc_kill_timer(app->main.hwnd, NC_TIMER_PERIODIC_UPDATE);
    }
    stop_engine_sync(app);
    if (app->main_ready) {
        nc_destroy(app->main.hwnd);
    }
}

static void try_install_pending(NcApp *app) {
    const char *version_text;
    if (!app || app->update_install_started || !app->update_has_pending) {
        return;
    }
    if (!app->config.settings.auto_update || !app->config.settings.update_feed_url ||
        !app->config.settings.update_feed_url[0] || !app->update_has_pending_request ||
        app->update_pending_request_generation != app->update_generation ||
        !nc_ieq_ascii(app->update_pending_request_feed, app->config.settings.update_feed_url)) {
        nc_update_manifest_free(&app->update_pending_manifest);
        app->update_has_pending = false;
        app->update_has_pending_request = false;
        return;
    }
    if (!app_is_idle(app)) {
        if (!app->update_deferred_logged) {
            app->update_deferred_logged = true;
            nc_update_log("当前弱网运行中或有窗口打开，暂不安装，等空闲后再继续。");
        }
        return;
    }
    app->update_install_started = true;
    app->update_deferred_logged = false;
    version_text = app->update_pending_manifest.version ? app->update_pending_manifest.version : "";
    {
        char message[512];
        snprintf(message, sizeof(message), "开始自更新到版本 %s。", version_text);
        nc_update_log(message);
    }
    stop_engine_sync(app);
    {
        int64_t attempted_at = nc_now_unix();
        app->config.settings.last_update_attempt_unix = attempted_at;
        free(app->config.settings.last_attempted_version);
        app->config.settings.last_attempted_version = nc_strdup(version_text);
        save_config(app);
    }
    if (!nc_update_apply_self_update(app->update_pending_installer)) {
        app->update_install_started = false;
        nc_update_log("启动自更新失败，继续使用当前版本。");
        return;
    }
    if (nc_update_is_dry_run()) {
        nc_update_log("DRY RUN：保持当前进程运行，不重启。");
    } else {
        exit_for_update(app);
    }
}

static void drain_update_events(NcApp *app) {
    NcTickOutcome outcome;
    if (!app || !app->update_bus) {
        return;
    }
    while (update_bus_pop(app->update_bus, &outcome)) {
        bool current;
        app->update_busy = false;
        current = app->update_has_active &&
                  app->update_active_generation == app->update_generation &&
                  nc_ieq_ascii(app->update_active_feed,
                               app->config.settings.update_feed_url
                                   ? app->config.settings.update_feed_url
                                   : "");
        app->update_has_active = false;
        if (!current || !app->config.settings.auto_update) {
            nc_update_log("忽略来自旧更新设置的检查结果。");
            nc_tick_outcome_free(&outcome);
            maybe_tick_update(app);
            continue;
        }
        switch (outcome.kind) {
        case NC_TICK_CHECK_FAILED: {
            char message[2200];
            snprintf(message, sizeof(message), "检查更新失败：%s",
                     outcome.error ? outcome.error : "");
            nc_update_log(message);
            break;
        }
        case NC_TICK_UP_TO_DATE:
            app->config.settings.last_update_check_unix = outcome.now;
            save_config(app);
            nc_update_log("已是最新版本。");
            break;
        case NC_TICK_READY:
            app->config.settings.last_update_check_unix = outcome.now;
            nc_update_manifest_free(&app->update_pending_manifest);
            app->update_pending_manifest = outcome.manifest;
            memset(&outcome.manifest, 0, sizeof(outcome.manifest));
            nc_copy_wide(app->update_pending_installer,
                         sizeof(app->update_pending_installer) /
                             sizeof(app->update_pending_installer[0]),
                         outcome.installer);
            app->update_has_pending = true;
            app->update_has_pending_request = true;
            app->update_pending_request_generation = app->update_generation;
            snprintf(app->update_pending_request_feed, sizeof(app->update_pending_request_feed),
                     "%s", app->config.settings.update_feed_url);
            save_config(app);
            try_install_pending(app);
            break;
        case NC_TICK_DOWNLOAD_FAILED:
            app->config.settings.last_update_check_unix = outcome.now;
            app->config.settings.last_update_attempt_unix = outcome.now;
            free(app->config.settings.last_attempted_version);
            app->config.settings.last_attempted_version =
                nc_strdup(outcome.manifest.version ? outcome.manifest.version : "");
            save_config(app);
            break;
        default:
            break;
        }
        nc_tick_outcome_free(&outcome);
    }
}

static void delete_profile_by_id(NcApp *app, const char *id) {
    if (!app || app->is_running || app->is_busy) {
        return;
    }
    nc_config_delete_profile(&app->config, id);
    save_config(app);
    bind_profiles(app);
    update_ui(app);
}

static void delete_active_profile(NcApp *app) {
    NcProfile *profile;
    if (!app || app->is_running || app->is_busy) {
        return;
    }
    profile = nc_config_active_profile(&app->config);
    if (!profile) {
        return;
    }
    {
        Str message;
        int answer;
        str_init(&message);
        str_addf(&message, "确定删除配置“%s”吗？", profile->name ? profile->name : "");
        answer = nc_app_show_message(app, str_cstr(&message), "删除配置",
                                     MB_YESNO | MB_ICONQUESTION);
        str_free(&message);
        if (answer != IDYES) {
            return;
        }
    }
    delete_profile_by_id(app, profile->id);
}

static void hide_to_tray(NcApp *app) {
    if (!app || !app->main_ready) {
        return;
    }
    nc_show(app->main.hwnd, SW_HIDE);
    if (!app->hide_balloon_shown) {
        app->hide_balloon_shown = true;
        show_balloon(app, "仍在后台运行", "点击托盘图标可重新打开；退出请使用托盘菜单。", NIIF_INFO);
    }
}

static void show_from_tray(NcApp *app) {
    if (!app || !app->main_ready) {
        return;
    }
    nc_show(app->main.hwnd, SW_SHOW);
    nc_set_foreground(app->main.hwnd);
}

static void exit_app(NcApp *app) {
    if (!app) {
        return;
    }
    app->exit_requested = true;
    if (app->main_ready) {
        nc_kill_timer(app->main.hwnd, NC_TIMER_STATS);
        nc_kill_timer(app->main.hwnd, NC_TIMER_STARTUP_UPDATE);
        nc_kill_timer(app->main.hwnd, NC_TIMER_PERIODIC_UPDATE);
    }
    stop_engine_sync(app);
    if (app->main_ready) {
        nc_destroy(app->main.hwnd);
    }
}

static void on_profile_changed(NcApp *app) {
    LRESULT index;
    if (!app || app->suppress_profile_change || !app->main_ready) {
        return;
    }
    index = SendMessageW(app->main.controls.profile_combo, CB_GETCURSEL, 0, 0);
    if (index < 0 || (size_t)index >= app->config.profile_count) {
        return;
    }
    free(app->config.active_profile_id);
    app->config.active_profile_id = nc_strdup(app->config.profiles[index].id);
    save_config(app);
}

static void on_tray_command(NcApp *app, NcTrayCommand command) {
    switch (command) {
    case NC_TRAY_COMMAND_SHOW:
        show_from_tray(app);
        break;
    case NC_TRAY_COMMAND_TOGGLE:
        if (app->is_busy) {
            return;
        }
        if (app->is_running) {
            stop_async(app, false);
        } else {
            start_async(app);
        }
        break;
    case NC_TRAY_COMMAND_SETTINGS:
        app->pending_action = NC_ACTION_CHANGE_SETTINGS;
        break;
    case NC_TRAY_COMMAND_EXIT:
        exit_app(app);
        break;
    default:
        break;
    }
}

static void on_dpi_changed(NcApp *app, WPARAM wparam, LPARAM lparam) {
    UINT new_dpi = (UINT)wparam & 0xFFFF;
    RECT *suggested;
    HWND child;
    double new_scale;
    if (!app || !app->main_ready || new_dpi == 0) {
        return;
    }
    if (app->dialog) {
        switch (app->dialog_kind) {
        case NC_DIALOG_EDITOR: {
            NcProfileEditor *editor = (NcProfileEditor *)app->dialog;
            nc_app_cleanup_dialog_widgets(app, nc_profile_editor_hwnd(editor));
            nc_destroy(nc_profile_editor_hwnd(editor));
            nc_profile_editor_free(editor);
            break;
        }
        case NC_DIALOG_SETTINGS: {
            NcSettingsDialog *dialog = (NcSettingsDialog *)app->dialog;
            nc_app_cleanup_dialog_widgets(app, nc_settings_dialog_hwnd(dialog));
            nc_destroy(nc_settings_dialog_hwnd(dialog));
            nc_settings_dialog_free(dialog);
            break;
        }
        case NC_DIALOG_SELECTOR: {
            NcProcessSelector *selector = (NcProcessSelector *)app->dialog;
            nc_app_cleanup_dialog_widgets(app, nc_process_selector_hwnd(selector));
            nc_destroy(nc_process_selector_hwnd(selector));
            nc_process_selector_free(selector);
            break;
        }
        default:
            break;
        }
        app->dialog = NULL;
        app->dialog_kind = NC_DIALOG_NONE;
    }
    child = GetWindow(app->main.hwnd, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        nc_destroy(child);
        child = next;
    }
    nc_widgets_clear(&app->widgets);
    nc_fonts_destroy(&app->fonts);
    new_scale = nc_fit_dialog_scale(app->main.hwnd, NC_MAIN_CLIENT_WIDTH, NC_MAIN_CLIENT_HEIGHT,
                                    (double)new_dpi / 96.0);
    app->main.scale = new_scale;
    app->main.dpi = (UINT)(new_scale * 96.0 + 0.5);
    nc_fonts_init(&app->fonts, app->main.dpi);
    nc_main_window_build(&app->main, &app->widgets, &app->fonts);
    nc_main_window_apply_fonts(&app->main, &app->fonts);
    nc_widget_apply_fonts(&app->widgets, &app->fonts);
    suggested = (RECT *)lparam;
    if (suggested) {
        nc_set_window_pos(app->main.hwnd, suggested->left, suggested->top,
                          suggested->right - suggested->left, suggested->bottom - suggested->top);
    }
    bind_profiles(app);
    update_ui(app);
    update_stats(app);
}

static bool on_main_message(NcApp *app, HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                            LRESULT *result) {
    switch (message) {
    case WM_ERASEBKGND:
        *result = 1;
        return true;
    case WM_PAINT:
        *result = nc_app_paint_surface(app, hwnd);
        return true;
    case WM_DRAWITEM:
        *result = nc_app_draw_item(app, lparam);
        return true;
    case WM_CTLCOLORSTATIC:
        nc_set_bk_color((HDC)wparam, NC_COLOR_CARD);
        nc_set_text_color((HDC)wparam, NC_COLOR_TEXT_DARK);
        *result = (LRESULT)app->white_brush;
        return true;
    case WM_CTLCOLORBTN: {
        uint32_t background;
        uint32_t foreground;
        if (nc_app_button_colors(app, (HWND)lparam, &background, &foreground)) {
            nc_set_dc_brush_color((HDC)wparam, background);
            nc_set_text_color((HDC)wparam, foreground);
            *result = (LRESULT)nc_dc_brush();
            return true;
        }
        return false;
    }
    case WM_COMMAND: {
        int id = (int)LOWORD(wparam);
        UINT code = HIWORD(wparam);
        switch (id) {
        case NC_ID_MAIN_PROFILE_COMBO:
            if (code == CBN_SELCHANGE) {
                on_profile_changed(app);
            }
            break;
        case NC_ID_MAIN_NEW:
            app->pending_action = NC_ACTION_CREATE_PROFILE;
            break;
        case NC_ID_MAIN_EDIT:
            app->pending_action = NC_ACTION_EDIT_PROFILE;
            break;
        case NC_ID_MAIN_DELETE:
            delete_active_profile(app);
            break;
        case NC_ID_MAIN_PROCESS_BUTTON:
            app->pending_action = NC_ACTION_SELECT_PROCESSES;
            break;
        case NC_ID_MAIN_TOGGLE:
            if (!app->is_busy) {
                if (app->is_running) {
                    stop_async(app, false);
                } else {
                    start_async(app);
                }
            }
            break;
        case NC_ID_MAIN_SETTINGS:
            app->pending_action = NC_ACTION_CHANGE_SETTINGS;
            break;
        case NC_ID_MAIN_MINIMIZE:
            hide_to_tray(app);
            break;
        case NC_ID_MAIN_EXIT:
            exit_app(app);
            break;
        default:
            break;
        }
        *result = 0;
        return true;
    }
    case WM_TIMER:
        if (wparam == NC_TIMER_STATS) {
            update_stats(app);
        } else if (wparam == NC_TIMER_STARTUP_UPDATE) {
            nc_kill_timer(hwnd, NC_TIMER_STARTUP_UPDATE);
            maybe_tick_update(app);
        } else if (wparam == NC_TIMER_PERIODIC_UPDATE) {
            maybe_tick_update(app);
        }
        *result = 0;
        return true;
    case WM_HOTKEY:
        if ((int)wparam == NC_HOTKEY_ID && !app->is_busy) {
            if (app->is_running) {
                stop_async(app, false);
            } else {
                start_async(app);
            }
        }
        *result = 0;
        return true;
    case NC_WM_APP_EVENTS:
        drain_events(app);
        drain_update_events(app);
        *result = 0;
        return true;
    case NC_WM_APP_TRAY:
        if (app->tray_ready) {
            NcTrayCommand command = nc_tray_handle_message(&app->tray, lparam, app->is_running);
            if (command != NC_TRAY_COMMAND_NONE) {
                on_tray_command(app, command);
            }
        }
        *result = 0;
        return true;
    case WM_CLOSE:
        if (app->exit_requested) {
            nc_destroy(hwnd);
        } else {
            hide_to_tray(app);
        }
        *result = 0;
        return true;
    case WM_DPICHANGED:
        on_dpi_changed(app, wparam, lparam);
        *result = 0;
        return true;
    case WM_DESTROY:
        nc_kill_timer(hwnd, NC_TIMER_STATS);
        nc_kill_timer(hwnd, NC_TIMER_STARTUP_UPDATE);
        nc_kill_timer(hwnd, NC_TIMER_PERIODIC_UPDATE);
        if (app->tray_ready) {
            nc_tray_destroy(&app->tray);
            app->tray_ready = false;
        }
        nc_unregister_hotkey(hwnd, NC_HOTKEY_ID);
        app->hotkey_registered = false;
        stop_engine_sync(app);
        nc_fonts_destroy(&app->fonts);
        if (app->white_brush) {
            nc_delete_object(app->white_brush);
            app->white_brush = NULL;
        }
        PostQuitMessage(0);
        *result = 0;
        return true;
    default:
        if (app->taskbar_message != 0 && message == app->taskbar_message) {
            if (app->tray_ready) {
                nc_tray_restore(&app->tray);
            }
            *result = 0;
            return true;
        }
        return false;
    }
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    LRESULT result = 0;
    if (!g_app) {
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (g_modal_active) {
        switch (message) {
        case NC_WM_APP_EVENTS:
            g_deferred_events = true;
            return 0;
        case WM_TIMER:
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, NULL);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }
    if (message == WM_NCCREATE) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (on_main_message(g_app, hwnd, message, wparam, lparam, &result)) {
        if (g_app->pending_action != NC_ACTION_NONE) {
            NcMainAction action = g_app->pending_action;
            g_app->pending_action = NC_ACTION_NONE;
            execute_main_action(g_app, action);
        }
        return result;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static LRESULT dispatch_dialog(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    NcApp *app = g_app;
    LRESULT result = 0;
    bool handled = false;
    if (!app || !app->dialog) {
        if (message == WM_PAINT) {
            PostMessageW(hwnd, message, wparam, lparam);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (app->dialog_kind) {
    case NC_DIALOG_EDITOR: {
        NcProfileEditor *editor = (NcProfileEditor *)app->dialog;
        if (nc_profile_editor_hwnd(editor) == hwnd) {
            handled = nc_profile_editor_message(editor, app, message, wparam, lparam, &result);
        }
        break;
    }
    case NC_DIALOG_SETTINGS: {
        NcSettingsDialog *dialog = (NcSettingsDialog *)app->dialog;
        if (nc_settings_dialog_hwnd(dialog) == hwnd) {
            handled = nc_settings_dialog_message(dialog, app, message, wparam, lparam, &result);
        }
        break;
    }
    case NC_DIALOG_SELECTOR: {
        NcProcessSelector *selector = (NcProcessSelector *)app->dialog;
        if (nc_process_selector_hwnd(selector) == hwnd) {
            handled = nc_process_selector_message(selector, app, message, wparam, lparam, &result);
        }
        break;
    }
    default:
        break;
    }
    return handled ? result : DefWindowProcW(hwnd, message, wparam, lparam);
}

static LRESULT CALLBACK editor_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return dispatch_dialog(hwnd, message, wparam, lparam);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return dispatch_dialog(hwnd, message, wparam, lparam);
}

static LRESULT CALLBACK selector_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return dispatch_dialog(hwnd, message, wparam, lparam);
}

LRESULT nc_app_dialog_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return dispatch_dialog(hwnd, message, wparam, lparam);
}

void nc_app_settings_hotkey_pressed(NcApp *app, uint32_t modifiers, int32_t key) {
    if (!app) {
        app = g_app;
    }
    if (app && app->dialog_kind == NC_DIALOG_SETTINGS) {
        nc_settings_set_candidate((NcSettingsDialog *)app->dialog, app, modifiers, key);
    }
}

void nc_app_settings_request_cancel(NcApp *app) {
    if (!app) {
        app = g_app;
    }
    if (app && app->dialog_kind == NC_DIALOG_SETTINGS) {
        nc_settings_cancel((NcSettingsDialog *)app->dialog, app);
    }
}

void nc_app_settings_request_confirm(NcApp *app) {
    if (!app) {
        app = g_app;
    }
    if (app && app->dialog_kind == NC_DIALOG_SETTINGS) {
        nc_settings_confirm((NcSettingsDialog *)app->dialog, app);
    }
}

static bool begin_modal(void) {
    if (g_modal_active) {
        return false;
    }
    g_deferred_events = false;
    g_modal_active = true;
    return true;
}

static void end_modal(NcApp *app) {
    g_modal_active = false;
    if (app && app->main_ready) {
        if (g_deferred_events) {
            g_deferred_events = false;
            PostMessageW(app->main.hwnd, NC_WM_APP_EVENTS, 0, 0);
        }
        InvalidateRect(app->main.hwnd, NULL, TRUE);
    }
}

static void build_existing_names(const NcApp *app, const char *exclude_id, NcStringList *names) {
    size_t index;
    nc_string_list_init(names);
    for (index = 0; index < app->config.profile_count; index++) {
        const NcProfile *profile = &app->config.profiles[index];
        if (exclude_id && profile->id && strcmp(profile->id, exclude_id) == 0) {
            continue;
        }
        nc_string_list_push(names, profile->name ? profile->name : "");
    }
}

static void execute_create_profile(NcApp *app) {
    NcStringList names;
    NcProfileEditor *editor;
    NcEditorOutcome outcome;
    HWND hwnd;
    if (!app || app->dialog || app->is_busy || app->is_running) {
        return;
    }
    build_existing_names(app, NULL, &names);
    editor = nc_profile_editor_create(app, NULL, false, &names);
    nc_string_list_free(&names);
    if (!editor) {
        return;
    }
    hwnd = nc_profile_editor_hwnd(editor);
    app->dialog = editor;
    app->dialog_kind = NC_DIALOG_EDITOR;
    if (!begin_modal()) {
        nc_destroy(hwnd);
        nc_profile_editor_free(editor);
        app->dialog = NULL;
        app->dialog_kind = NC_DIALOG_NONE;
        return;
    }
    nc_run_modal(nc_app_main_hwnd(app), hwnd);
    end_modal(app);
    app->dialog = NULL;
    app->dialog_kind = NC_DIALOG_NONE;
    if (nc_profile_editor_take_outcome(editor, &outcome) &&
        outcome.kind == NC_EDITOR_OUTCOME_SAVED) {
        NcProfile saved = outcome.profile;
        if (app->is_running || app->is_busy) {
            nc_config_free_profile(&saved);
        } else {
            free(app->config.active_profile_id);
            app->config.active_profile_id = nc_strdup(saved.id);
            nc_config_add_profile(&app->config, &saved);
            nc_config_free_profile(&saved);
            save_config(app);
            bind_profiles(app);
            update_ui(app);
        }
    }
    nc_profile_editor_free(editor);
}

static void execute_edit_profile(NcApp *app) {
    NcProfile *active;
    NcProfile current;
    bool was_running;
    NcStringList names;
    NcProfileEditor *editor;
    NcEditorOutcome outcome;
    HWND hwnd;
    if (!app || app->dialog || app->is_busy) {
        return;
    }
    active = nc_config_active_profile(&app->config);
    if (!active) {
        return;
    }
    memset(&current, 0, sizeof(current));
    if (!nc_config_copy_profile(active, &current)) {
        return;
    }
    was_running = app->is_running;
    build_existing_names(app, current.id, &names);
    editor = nc_profile_editor_create(app, &current, !was_running, &names);
    nc_string_list_free(&names);
    if (!editor) {
        nc_config_free_profile(&current);
        return;
    }
    hwnd = nc_profile_editor_hwnd(editor);
    app->dialog = editor;
    app->dialog_kind = NC_DIALOG_EDITOR;
    if (!begin_modal()) {
        nc_destroy(hwnd);
        nc_profile_editor_free(editor);
        nc_config_free_profile(&current);
        app->dialog = NULL;
        app->dialog_kind = NC_DIALOG_NONE;
        return;
    }
    nc_run_modal(nc_app_main_hwnd(app), hwnd);
    end_modal(app);
    app->dialog = NULL;
    app->dialog_kind = NC_DIALOG_NONE;
    if (nc_profile_editor_take_outcome(editor, &outcome)) {
        if (outcome.kind == NC_EDITOR_OUTCOME_DELETED) {
            delete_profile_by_id(app, current.id);
        } else if (outcome.kind == NC_EDITOR_OUTCOME_SAVED && !app->is_busy) {
            size_t index = nc_config_find_profile_index(&app->config, current.id);
            if (index != (size_t)-1) {
                NcProfile saved = outcome.profile;
                nc_config_replace_profile(&app->config, index, &saved);
                free(app->config.active_profile_id);
                app->config.active_profile_id = nc_strdup(saved.id);
                nc_config_free_profile(&saved);
                save_config(app);
                bind_profiles(app);
                update_ui(app);
                if (was_running) {
                    stop_engine_sync(app);
                    update_ui(app);
                    start_async(app);
                }
            }
        }
    } else {
        update_ui(app);
    }
    nc_profile_editor_free(editor);
    nc_config_free_profile(&current);
}

static void execute_select_processes(NcApp *app) {
    NcProcessSelector *selector;
    NcStringList names;
    HWND hwnd;
    if (!app || app->dialog || app->is_running || app->is_busy) {
        return;
    }
    selector = nc_process_selector_create(app);
    if (!selector) {
        return;
    }
    hwnd = nc_process_selector_hwnd(selector);
    app->dialog = selector;
    app->dialog_kind = NC_DIALOG_SELECTOR;
    if (!begin_modal()) {
        nc_destroy(hwnd);
        nc_process_selector_free(selector);
        app->dialog = NULL;
        app->dialog_kind = NC_DIALOG_NONE;
        return;
    }
    nc_run_modal(nc_app_main_hwnd(app), hwnd);
    end_modal(app);
    app->dialog = NULL;
    app->dialog_kind = NC_DIALOG_NONE;
    nc_string_list_init(&names);
    if (nc_process_selector_take_outcome(selector, &names)) {
        if (names.count > NC_MAX_PROCESS_COUNT) {
            nc_app_show_message(app, "目标进程最多 64 个。", "目标进程", MB_OK | MB_ICONWARNING);
            nc_string_list_free(&names);
            update_ui(app);
        } else {
            nc_string_list_free(&app->config.settings.process_names);
            app->config.settings.process_names = names;
            save_config(app);
            update_ui(app);
        }
    } else {
        nc_string_list_free(&names);
        update_ui(app);
    }
    nc_process_selector_free(selector);
}

static void execute_change_settings(NcApp *app) {
    NcHotkey old_hotkey;
    NcSettingsDialog *dialog;
    NcSettingsOutcome outcome;
    HWND hwnd;
    bool has_outcome;
    if (!app || app->dialog || app->is_busy) {
        return;
    }
    old_hotkey = app->config.hotkey;
    if (app->main_ready) {
        nc_unregister_hotkey(app->main.hwnd, NC_HOTKEY_ID);
    }
    app->hotkey_registered = false;
    dialog = nc_settings_dialog_create(app);
    if (!dialog) {
        register_hotkey(app, app->config.hotkey.modifiers, app->config.hotkey.key);
        update_ui(app);
        return;
    }
    hwnd = nc_settings_dialog_hwnd(dialog);
    app->dialog = dialog;
    app->dialog_kind = NC_DIALOG_SETTINGS;
    if (!begin_modal()) {
        nc_destroy(hwnd);
        nc_settings_dialog_free(dialog);
        app->dialog = NULL;
        app->dialog_kind = NC_DIALOG_NONE;
        register_hotkey(app, app->config.hotkey.modifiers, app->config.hotkey.key);
        update_ui(app);
        return;
    }
    nc_run_modal(nc_app_main_hwnd(app), hwnd);
    end_modal(app);
    app->dialog = NULL;
    app->dialog_kind = NC_DIALOG_NONE;
    memset(&outcome, 0, sizeof(outcome));
    has_outcome = nc_settings_dialog_take_outcome(dialog, &outcome);
    nc_settings_dialog_free(dialog);
    if (!has_outcome) {
        if (!app->hotkey_registered) {
            register_hotkey(app, app->config.hotkey.modifiers, app->config.hotkey.key);
        }
        update_ui(app);
        return;
    }
    register_hotkey(app, outcome.modifiers, outcome.key);
    if (!app->hotkey_registered) {
        char new_error[256];
        Str message;
        snprintf(new_error, sizeof(new_error), "%s", app->hotkey_error);
        register_hotkey(app, old_hotkey.modifiers, old_hotkey.key);
        str_init(&message);
        str_addf(&message, "新的快捷键无法注册：%s\r\n%s", new_error,
                 app->hotkey_registered ? "已保留原快捷键。"
                                        : "原快捷键也无法重新注册，请尽快重新设置。");
        nc_app_show_message(app, str_cstr(&message), "快捷键", MB_OK | MB_ICONWARNING);
        str_free(&message);
        free(outcome.update_feed_url);
        update_ui(app);
        return;
    }
    {
        bool update_changed =
            outcome.auto_update != app->config.settings.auto_update ||
            !nc_ieq_ascii(outcome.update_feed_url ? outcome.update_feed_url : "",
                          app->config.settings.update_feed_url
                              ? app->config.settings.update_feed_url
                              : "");
        if (update_changed) {
            app->update_generation++;
            app->update_has_active = false;
            app->update_has_pending_request = false;
            nc_update_manifest_free(&app->update_pending_manifest);
            app->update_has_pending = false;
        }
    }
    app->config.hotkey.modifiers = outcome.modifiers;
    app->config.hotkey.key = outcome.key;
    app->config.settings.max_run_minutes = outcome.max_run_minutes;
    app->config.settings.max_queue_delay_ms = outcome.max_queue_delay_ms;
    app->config.settings.auto_update = outcome.auto_update;
    free(app->config.settings.update_feed_url);
    app->config.settings.update_feed_url =
        nc_strdup(outcome.update_feed_url ? outcome.update_feed_url : "");
    free(outcome.update_feed_url);
    if (app->is_running) {
        schedule_auto_stop(app);
    }
    save_config(app);
    update_ui(app);
    maybe_tick_update(app);
}

static void execute_main_action(NcApp *app, NcMainAction action) {
    switch (action) {
    case NC_ACTION_CREATE_PROFILE:
        execute_create_profile(app);
        break;
    case NC_ACTION_EDIT_PROFILE:
        execute_edit_profile(app);
        break;
    case NC_ACTION_SELECT_PROCESSES:
        execute_select_processes(app);
        break;
    case NC_ACTION_CHANGE_SETTINGS:
        execute_change_settings(app);
        break;
    default:
        break;
    }
}

static bool register_classes(void) {
    return nc_register_class(L"NCMain", main_proc) &&
           nc_register_class(L"NCProfileEditor", editor_proc) &&
           nc_register_class(L"NCSettings", settings_proc) &&
           nc_register_class(L"NCProcessSelector", selector_proc);
}

static bool app_build(NcApp *app) {
    HWND hwnd;
    if (!register_classes()) {
        return false;
    }
    hwnd = nc_main_window_create();
    if (!hwnd) {
        return false;
    }
    nc_main_window_init(&app->main, hwnd);
    app->main_ready = true;
    nc_fonts_init(&app->fonts, app->main.dpi);
    nc_main_window_build(&app->main, &app->widgets, &app->fonts);
    nc_main_window_apply_fonts(&app->main, &app->fonts);
    nc_widget_apply_fonts(&app->widgets, &app->fonts);
    nc_tray_init(&app->tray, hwnd);
    app->tray_ready = true;
    app->taskbar_message = RegisterWindowMessageW(L"TaskbarCreated");
    app->events = nc_engine_events_new(hwnd, NC_WM_APP_EVENTS);
    if (!app->events) {
        return false;
    }
    app->update_bus = update_bus_new(hwnd);
    app->engine = nc_conditioner_new(app->events);
    if (!app->engine) {
        return false;
    }
    register_hotkey(app, app->config.hotkey.modifiers, app->config.hotkey.key);
    nc_set_timer(hwnd, NC_TIMER_STATS, 500);
    nc_set_timer(hwnd, NC_TIMER_STARTUP_UPDATE, 5000);
    nc_set_timer(hwnd, NC_TIMER_PERIODIC_UPDATE, 6 * 60 * 60 * 1000);
    bind_profiles(app);
    update_ui(app);
    update_stats(app);
    nc_show(hwnd, SW_SHOW);
    nc_set_foreground(hwnd);
    return true;
}

static void app_after_show(NcApp *app) {
    if (app->load_warning) {
        nc_app_show_message(app, app->load_warning, "配置提示", MB_OK | MB_ICONWARNING);
    }
    if (!app->hotkey_registered) {
        Str message;
        str_init(&message);
        str_addf(&message, "全局快捷键注册失败：%s\r\n你也可以用窗口按钮启动/停止。",
                 app->hotkey_error);
        nc_app_show_message(app, str_cstr(&message), "快捷键", MB_OK | MB_ICONWARNING);
        str_free(&message);
    }
}

static void app_free(NcApp *app) {
    if (!app) {
        return;
    }
    if (app->tray_ready) {
        nc_tray_destroy(&app->tray);
    }
    EnterCriticalSection(&app->worker_lock);
    while (app->worker_count > 0) {
        SleepConditionVariableCS(&app->worker_cv, &app->worker_lock, INFINITE);
    }
    LeaveCriticalSection(&app->worker_lock);
    nc_fonts_destroy(&app->fonts);
    nc_widgets_free(&app->widgets);
    if (app->update_bus) {
        update_bus_release(app->update_bus);
        app->update_bus = NULL;
    }
    nc_update_manifest_free(&app->update_pending_manifest);
    DeleteCriticalSection(&app->worker_lock);
    if (app->engine) {
        nc_conditioner_free(app->engine);
    }
    if (app->events) {
        nc_engine_events_free(app->events);
    }
    if (app->white_brush) {
        nc_delete_object(app->white_brush);
    }
    nc_config_free(&app->config);
    free(app->load_warning);
    free(app);
}

int nc_app_run(void) {
    NcApp *app;
    int code;
    char warning[2048];
    app = (NcApp *)calloc(1, sizeof(NcApp));
    if (!app) {
        return 1;
    }
    g_app = app;
    nc_widgets_init(&app->widgets);
    InitializeCriticalSection(&app->worker_lock);
    InitializeConditionVariable(&app->worker_cv);
    nc_store_init(&app->store);
    warning[0] = 0;
    if (nc_store_load(&app->store, &app->config, warning, sizeof(warning))) {
        if (warning[0]) {
            app->load_warning = nc_strdup(warning);
        }
    } else {
        nc_config_fresh(&app->config);
        {
            size_t removed = 0;
            nc_config_sanitize(&app->config, nc_now_unix(), &removed);
        }
    }
    app->white_brush = nc_create_white_brush();
    if (!app_build(app)) {
        app_free(app);
        g_app = NULL;
        return 1;
    }
    app_after_show(app);
    code = nc_message_loop();
    app_free(app);
    g_app = NULL;
    return code;
}
