#include "app/app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/engine_runner.h"
#include "config/config.h"
#include "config/store.h"
#include "core/hotkey.h"
#include "core/str.h"
#include "core/time.h"
#include "core/win.h"
#include "ui/ids.h"
#include "ui/main_window.h"
#include "ui/process_selector.h"
#include "ui/profile_editor.h"
#include "ui/settings.h"
#include "ui/tray.h"
#include "ui/ui_host.h"
#include "update/service.h"

#define NC_TIMER_STATS 1
#define NC_TIMER_STARTUP_UPDATE 2
#define NC_TIMER_PERIODIC_UPDATE 3
#define NC_HOTKEY_ID 0x4E43

typedef enum {
    NC_ACTION_NONE = 0,
    NC_ACTION_CREATE_PROFILE,
    NC_ACTION_EDIT_PROFILE,
    NC_ACTION_SELECT_PROCESSES,
    NC_ACTION_CHANGE_SETTINGS
} NcMainAction;

typedef struct NcApp {
    NcConfig config;
    NcStore store;
    char *load_warning;
    NcEngineRunner *runner;
    NcUpdateService *updater;
    NcWidgets widgets;
    NcFonts fonts;
    NcMainWindow main;
    bool main_ready;
    NcTray tray;
    bool tray_ready;
    UINT taskbar_message;
    bool hotkey_registered;
    char hotkey_error[256];
    bool exit_requested;
    bool hide_balloon_shown;
    bool remote_warning_shown;
    bool suppress_profile_change;
    bool modal_active;
    bool deferred_events;
    NcMainAction pending_action;
    HBRUSH white_brush;
} NcApp;

static void update_ui(NcApp *app);
static void update_stats(NcApp *app);
static void exit_app(NcApp *app);

static NcUiHost make_ui_host(NcApp *app) {
    NcUiHost host;
    host.owner = (app && app->main_ready) ? app->main.hwnd : NULL;
    host.scale = (app && app->main_ready) ? app->main.scale : 1.0;
    host.white_brush = app ? app->white_brush : NULL;
    return host;
}

static int show_message(NcApp *app, const char *text, const char *title, UINT flags) {
    NcUiHost host = make_ui_host(app);
    return nc_ui_host_message(&host, text, title, flags);
}

static void show_balloon(NcApp *app, const char *title, const char *text, UINT flags) {
    if (app && app->tray_ready) {
        nc_tray_balloon(&app->tray, title, text, flags);
    }
}

static void save_config(NcApp *app) {
    NcErr error;
    if (!app) {
        return;
    }
    if (!nc_store_save(&app->store, &app->config, &error)) {
        char message[1200];
        snprintf(message, sizeof(message), "保存配置失败：%s", error.msg);
        show_message(app, message, "NetConditioner", MB_OK | MB_ICONERROR);
    }
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
    HWND hwnd = (app && app->main_ready) ? app->main.hwnd : NULL;
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
    NcProfile *active;
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
    active = nc_config_active_profile(&app->config);
    if (active) {
        size_t found = nc_config_find_profile_index(&app->config, active->id);
        selection = found == (size_t)-1 ? 0 : (int)found;
        free(app->config.active_profile_id);
        app->config.active_profile_id = nc_strdup(active->id ? active->id : "");
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
    if (nc_engine_runner_is_running(app->runner)) {
        NcStatsSnapshot uplink;
        NcStatsSnapshot downlink;
        Str up_text;
        Str down_text;
        nc_engine_runner_stats(app->runner, &uplink, &downlink);
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
        if (nc_engine_runner_auto_stop_due(app->runner)) {
            nc_engine_runner_stop_sync(app->runner);
            update_ui(app);
            show_balloon(app, "已自动恢复网络", "已达到最长运行时间", NIIF_INFO);
            update_stats(app);
        }
    } else {
        nc_widget_set_text(&app->widgets, controls->uplink_stats, "上行   发送 0   丢弃 0   队列 0");
        nc_widget_set_text(&app->widgets, controls->downlink_stats, "下行   发送 0   丢弃 0   队列 0");
    }
}

static void update_ui(NcApp *app) {
    NcMainControls *controls;
    bool running;
    bool busy;
    const char *toggle_text;
    uint32_t toggle_color;
    bool toggle_enabled;
    if (!app || !app->main_ready) {
        return;
    }
    running = nc_engine_runner_is_running(app->runner);
    busy = nc_engine_runner_is_busy(app->runner);
    controls = &app->main.controls;
    if (busy) {
        toggle_text = "正在应用…";
        toggle_color = NC_COLOR_GRAY;
        toggle_enabled = false;
    } else if (running) {
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
    if (busy) {
        nc_widget_set_text_color(&app->widgets, controls->status_dot, NC_COLOR_ORANGE);
        nc_widget_set_text(&app->widgets, controls->status_label, "正在切换…");
        nc_widget_set_text(&app->widgets, controls->status_detail, "请稍候");
    } else if (running) {
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
        bool enabled_idle = !running && !busy;
        nc_enable(controls->profile_combo, enabled_idle);
        nc_enable(controls->new_button, enabled_idle);
        nc_enable(controls->edit_button, !busy);
        nc_enable(controls->delete_button, enabled_idle);
        nc_enable(controls->process_button, enabled_idle);
        nc_enable(controls->settings_link, !busy);
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
            nc_hotkey_format(app->config.hotkey.modifiers, app->config.hotkey.key, text, sizeof(text));
        } else {
            snprintf(text, sizeof(text), "未注册（点击设置）");
        }
        nc_widget_set_text(&app->widgets, controls->hotkey_label, text);
    }
    if (app->tray_ready) {
        nc_tray_set_running(&app->tray, running);
    }
}

static void request_start(NcApp *app) {
    NcProfile *source;
    NcErr error;
    if (!app || nc_engine_runner_is_busy(app->runner) || nc_engine_runner_is_running(app->runner)) {
        return;
    }
    source = nc_config_active_profile(&app->config);
    if (!source) {
        return;
    }
    if (!nc_profile_validate(source, &error)) {
        show_message(app, error.msg, "配置无效", MB_OK | MB_ICONWARNING);
        return;
    }
    if (nc_profile_all_zero(source)) {
        int answer = show_message(app, "当前配置所有参数均为 0，启动后不会产生弱网效果。仍要启动吗？",
                                  "配置为空", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            return;
        }
    }
    if (nc_is_remote_session() && !app->remote_warning_shown) {
        int answer;
        app->remote_warning_shown = true;
        answer = show_message(
            app,
            "当前是远程桌面会话。开启弱网（尤其是高丢包/低带宽）可能导致远程连接卡顿或断开。\r\n\r\n"
            "建议先设置“最长运行时间”，确保断线后能自动恢复。仍要启动吗？",
            "远程会话提醒", MB_YESNO | MB_ICONWARNING);
        if (answer != IDYES) {
            return;
        }
    }
    nc_engine_runner_start_async(app->runner, source, &app->config.settings);
}

static void toggle_engine(NcApp *app) {
    if (!app || nc_engine_runner_is_busy(app->runner)) {
        return;
    }
    if (nc_engine_runner_is_running(app->runner)) {
        nc_engine_runner_stop_async(app->runner, false);
    } else {
        request_start(app);
    }
}

/* Engine runner callbacks. */
static void runner_on_state(void *userdata) {
    update_ui((NcApp *)userdata);
}

static void runner_on_start_ok(void *userdata) {
    NcApp *app = (NcApp *)userdata;
    NcProfile *profile = nc_config_active_profile(&app->config);
    Str text;
    update_ui(app);
    update_stats(app);
    str_init(&text);
    str_addf(&text, "配置：%s", profile && profile->name ? profile->name : "");
    show_balloon(app, "弱网模拟已启动", str_cstr(&text), NIIF_INFO);
    str_free(&text);
}

static void runner_on_start_fail(void *userdata, const char *message) {
    NcApp *app = (NcApp *)userdata;
    Str text;
    update_ui(app);
    update_stats(app);
    str_init(&text);
    str_addf(&text, "启动弱网模拟失败：\r\n%s", message ? message : "");
    show_message(app, str_cstr(&text), "NetConditioner", MB_OK | MB_ICONERROR);
    str_free(&text);
}

static void runner_on_stop(void *userdata, bool silent) {
    NcApp *app = (NcApp *)userdata;
    update_ui(app);
    update_stats(app);
    if (!silent) {
        show_balloon(app, "已恢复网络", "弱网模拟已停止", NIIF_INFO);
    }
    nc_update_service_maybe_tick(app->updater, &app->config.settings);
}

static void runner_on_fault(void *userdata, const char *message) {
    NcApp *app = (NcApp *)userdata;
    update_ui(app);
    update_stats(app);
    show_balloon(app, "弱网模拟已停止", "引擎错误，网络已恢复", NIIF_ERROR);
    show_message(app, message ? message : "弱网模拟已停止", "弱网模拟已停止", MB_OK | MB_ICONERROR);
}

/* Update service host callbacks. */
static bool update_host_is_idle(void *userdata) {
    NcApp *app = (NcApp *)userdata;
    return app && !nc_engine_runner_is_running(app->runner) &&
           !nc_engine_runner_is_busy(app->runner) && !app->modal_active;
}

static void update_host_save_config(void *userdata) {
    save_config((NcApp *)userdata);
}

static void update_host_stop_engine(void *userdata) {
    NcApp *app = (NcApp *)userdata;
    nc_engine_runner_stop_sync(app->runner);
}

static void update_host_exit(void *userdata) {
    exit_app((NcApp *)userdata);
}

static void delete_profile_by_id(NcApp *app, const char *id) {
    if (!app || nc_engine_runner_is_running(app->runner) || nc_engine_runner_is_busy(app->runner)) {
        return;
    }
    nc_config_delete_profile(&app->config, id);
    save_config(app);
    bind_profiles(app);
    update_ui(app);
}

static void delete_active_profile(NcApp *app) {
    NcProfile *profile;
    Str message;
    int answer;
    if (!app || nc_engine_runner_is_running(app->runner) || nc_engine_runner_is_busy(app->runner)) {
        return;
    }
    profile = nc_config_active_profile(&app->config);
    if (!profile) {
        return;
    }
    str_init(&message);
    str_addf(&message, "确定删除配置“%s”吗？", profile->name ? profile->name : "");
    answer = show_message(app, str_cstr(&message), "删除配置", MB_YESNO | MB_ICONQUESTION);
    str_free(&message);
    if (answer == IDYES) {
        delete_profile_by_id(app, profile->id);
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

static bool begin_modal(NcApp *app) {
    if (!app || app->modal_active) {
        return false;
    }
    app->deferred_events = false;
    app->modal_active = true;
    return true;
}

static void end_modal(NcApp *app) {
    app->modal_active = false;
    if (app->main_ready) {
        if (app->deferred_events) {
            app->deferred_events = false;
            PostMessageW(app->main.hwnd, NC_WM_APP_EVENTS, 0, 0);
        }
        InvalidateRect(app->main.hwnd, NULL, TRUE);
    }
}

static void execute_create_profile(NcApp *app) {
    NcStringList names;
    NcEditorOutcome outcome;
    NcUiHost host;
    bool has_outcome;
    if (!app || nc_engine_runner_is_busy(app->runner) || nc_engine_runner_is_running(app->runner)) {
        return;
    }
    if (!begin_modal(app)) {
        return;
    }
    build_existing_names(app, NULL, &names);
    host = make_ui_host(app);
    has_outcome = nc_profile_editor_run(&host, NULL, false, &names, &outcome);
    nc_string_list_free(&names);
    end_modal(app);
    if (has_outcome && outcome.kind == NC_EDITOR_OUTCOME_SAVED) {
        if (nc_engine_runner_is_running(app->runner) || nc_engine_runner_is_busy(app->runner)) {
            nc_config_free_profile(&outcome.profile);
        } else {
            free(app->config.active_profile_id);
            app->config.active_profile_id = nc_strdup(outcome.profile.id);
            nc_config_add_profile(&app->config, &outcome.profile);
            nc_config_free_profile(&outcome.profile);
            save_config(app);
            bind_profiles(app);
            update_ui(app);
        }
    }
    nc_update_service_try_install(app->updater, &app->config.settings);
}

static void execute_edit_profile(NcApp *app) {
    NcProfile *active;
    NcProfile current;
    bool was_running;
    NcStringList names;
    NcEditorOutcome outcome;
    NcUiHost host;
    bool has_outcome;
    if (!app || nc_engine_runner_is_busy(app->runner)) {
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
    if (!begin_modal(app)) {
        nc_config_free_profile(&current);
        return;
    }
    was_running = nc_engine_runner_is_running(app->runner);
    build_existing_names(app, current.id, &names);
    host = make_ui_host(app);
    has_outcome = nc_profile_editor_run(&host, &current, !was_running, &names, &outcome);
    nc_string_list_free(&names);
    end_modal(app);
    if (has_outcome) {
        if (outcome.kind == NC_EDITOR_OUTCOME_DELETED) {
            delete_profile_by_id(app, current.id);
        } else if (outcome.kind == NC_EDITOR_OUTCOME_SAVED && !nc_engine_runner_is_busy(app->runner)) {
            size_t index = nc_config_find_profile_index(&app->config, current.id);
            if (index != (size_t)-1) {
                nc_config_replace_profile(&app->config, index, &outcome.profile);
                free(app->config.active_profile_id);
                app->config.active_profile_id = nc_strdup(outcome.profile.id);
                nc_config_free_profile(&outcome.profile);
                save_config(app);
                bind_profiles(app);
                update_ui(app);
                if (was_running) {
                    nc_engine_runner_stop_sync(app->runner);
                    update_ui(app);
                    request_start(app);
                }
            } else {
                nc_config_free_profile(&outcome.profile);
            }
        } else {
            nc_config_free_profile(&outcome.profile);
        }
    } else {
        update_ui(app);
    }
    nc_config_free_profile(&current);
    nc_update_service_try_install(app->updater, &app->config.settings);
}

static void execute_select_processes(NcApp *app) {
    NcStringList names;
    NcUiHost host;
    bool has_outcome;
    if (!app || nc_engine_runner_is_running(app->runner) || nc_engine_runner_is_busy(app->runner)) {
        return;
    }
    if (!begin_modal(app)) {
        return;
    }
    host = make_ui_host(app);
    has_outcome = nc_process_selector_run(&host, &app->config.settings.process_names, &names);
    end_modal(app);
    if (has_outcome) {
        if (names.count > NC_MAX_PROCESS_COUNT) {
            show_message(app, "目标进程最多 64 个。", "目标进程", MB_OK | MB_ICONWARNING);
            nc_string_list_free(&names);
        } else {
            nc_string_list_free(&app->config.settings.process_names);
            app->config.settings.process_names = names;
            save_config(app);
        }
    } else {
        nc_string_list_free(&names);
    }
    update_ui(app);
    nc_update_service_try_install(app->updater, &app->config.settings);
}

static void execute_change_settings(NcApp *app) {
    NcHotkey old_hotkey;
    NcSettingsOutcome outcome;
    NcUiHost host;
    bool has_outcome;
    if (!app || nc_engine_runner_is_busy(app->runner)) {
        return;
    }
    if (!begin_modal(app)) {
        return;
    }
    old_hotkey = app->config.hotkey;
    if (app->main_ready) {
        nc_unregister_hotkey(app->main.hwnd, NC_HOTKEY_ID);
    }
    app->hotkey_registered = false;
    host = make_ui_host(app);
    has_outcome = nc_settings_dialog_run(&host, &app->config, &outcome);
    end_modal(app);
    if (!has_outcome) {
        if (!app->hotkey_registered) {
            register_hotkey(app, app->config.hotkey.modifiers, app->config.hotkey.key);
        }
        update_ui(app);
        nc_update_service_try_install(app->updater, &app->config.settings);
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
        show_message(app, str_cstr(&message), "快捷键", MB_OK | MB_ICONWARNING);
        str_free(&message);
        free(outcome.update_feed_url);
        update_ui(app);
        return;
    }
    {
        bool update_changed =
            outcome.auto_update != app->config.settings.auto_update ||
            !nc_ieq_ascii(outcome.update_feed_url ? outcome.update_feed_url : "",
                          app->config.settings.update_feed_url ? app->config.settings.update_feed_url : "");
        app->config.hotkey.modifiers = outcome.modifiers;
        app->config.hotkey.key = outcome.key;
        app->config.settings.max_run_minutes = outcome.max_run_minutes;
        app->config.settings.max_queue_delay_ms = outcome.max_queue_delay_ms;
        app->config.settings.auto_update = outcome.auto_update;
        free(app->config.settings.update_feed_url);
        app->config.settings.update_feed_url = nc_strdup(outcome.update_feed_url ? outcome.update_feed_url : "");
        free(outcome.update_feed_url);
        if (nc_engine_runner_is_running(app->runner)) {
            nc_engine_runner_reschedule_auto_stop(app->runner, app->config.settings.max_run_minutes);
        }
        save_config(app);
        update_ui(app);
        if (update_changed) {
            nc_update_service_on_settings_changed(app->updater, &app->config.settings);
        } else {
            nc_update_service_maybe_tick(app->updater, &app->config.settings);
        }
    }
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
    nc_engine_runner_stop_sync(app->runner);
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
        toggle_engine(app);
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
    if (message != WM_CTLCOLOREDIT &&
        nc_ui_handle_chrome(&app->widgets, &app->fonts, app->white_brush, hwnd, message, wparam,
                            lparam, NC_COLOR_CARD, result)) {
        return true;
    }
    switch (message) {
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
            toggle_engine(app);
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
            nc_update_service_maybe_tick(app->updater, &app->config.settings);
        } else if (wparam == NC_TIMER_PERIODIC_UPDATE) {
            nc_update_service_maybe_tick(app->updater, &app->config.settings);
        }
        *result = 0;
        return true;
    case WM_HOTKEY:
        if ((int)wparam == NC_HOTKEY_ID) {
            toggle_engine(app);
        }
        *result = 0;
        return true;
    case NC_WM_APP_EVENTS:
        nc_engine_runner_drain(app->runner, app->config.settings.max_run_minutes);
        nc_update_service_drain(app->updater, &app->config.settings);
        *result = 0;
        return true;
    case NC_WM_APP_TRAY:
        if (app->tray_ready) {
            NcTrayCommand command = nc_tray_handle_message(&app->tray, lparam,
                                                           nc_engine_runner_is_running(app->runner));
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
        nc_update_service_detach_window(app->updater);
        if (app->tray_ready) {
            nc_tray_destroy(&app->tray);
            app->tray_ready = false;
        }
        nc_unregister_hotkey(hwnd, NC_HOTKEY_ID);
        app->hotkey_registered = false;
        nc_engine_runner_stop_sync(app->runner);
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
    NcApp *app;
    LRESULT result = 0;
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    app = (NcApp *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!app) {
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (app->modal_active) {
        switch (message) {
        case NC_WM_APP_EVENTS:
            app->deferred_events = true;
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
    if (on_main_message(app, hwnd, message, wparam, lparam, &result)) {
        if (app->pending_action != NC_ACTION_NONE) {
            NcMainAction action = app->pending_action;
            app->pending_action = NC_ACTION_NONE;
            execute_main_action(app, action);
        }
        return result;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static bool app_build(NcApp *app) {
    HWND hwnd;
    NcEngineRunnerCallbacks runner_cb;
    NcUpdateHost update_host;
    if (!nc_register_class(L"NCMain", main_proc)) {
        return false;
    }
    hwnd = nc_main_window_create(app);
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
    memset(&runner_cb, 0, sizeof(runner_cb));
    runner_cb.userdata = app;
    runner_cb.on_state_changed = runner_on_state;
    runner_cb.on_start_succeeded = runner_on_start_ok;
    runner_cb.on_start_failed = runner_on_start_fail;
    runner_cb.on_stop_finished = runner_on_stop;
    runner_cb.on_fault = runner_on_fault;
    app->runner = nc_engine_runner_new(hwnd, NC_WM_APP_EVENTS, &runner_cb);
    if (!app->runner) {
        return false;
    }
    memset(&update_host, 0, sizeof(update_host));
    update_host.userdata = app;
    update_host.is_idle = update_host_is_idle;
    update_host.save_config = update_host_save_config;
    update_host.stop_engine_sync = update_host_stop_engine;
    update_host.exit_for_update = update_host_exit;
    app->updater = nc_update_service_new(hwnd, NC_WM_APP_EVENTS, &update_host);
    if (!app->updater) {
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
        show_message(app, app->load_warning, "配置提示", MB_OK | MB_ICONWARNING);
    }
    if (!app->hotkey_registered) {
        Str message;
        str_init(&message);
        str_addf(&message, "全局快捷键注册失败：%s\r\n你也可以用窗口按钮启动/停止。", app->hotkey_error);
        show_message(app, str_cstr(&message), "快捷键", MB_OK | MB_ICONWARNING);
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
    nc_update_service_free(app->updater);
    nc_engine_runner_free(app->runner);
    nc_fonts_destroy(&app->fonts);
    nc_widgets_free(&app->widgets);
    if (app->white_brush) {
        nc_delete_object(app->white_brush);
    }
    nc_store_free(&app->store);
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
    nc_widgets_init(&app->widgets);
    nc_store_init(&app->store);
    warning[0] = 0;
    if (nc_store_load(&app->store, &app->config, warning, sizeof(warning))) {
        if (warning[0]) {
            app->load_warning = nc_strdup(warning);
        }
    } else {
        size_t removed = 0;
        nc_config_fresh(&app->config);
        nc_config_sanitize(&app->config, nc_now_unix(), &removed);
    }
    app->white_brush = nc_create_white_brush();
    if (!app_build(app)) {
        app_free(app);
        return 1;
    }
    app_after_show(app);
    code = nc_message_loop();
    app_free(app);
    return code;
}
