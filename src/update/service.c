#include "update/service.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/str.h"
#include "core/time.h"
#include "core/version.h"
#include "update/coordinator.h"
#include "update/update.h"

typedef struct UpdateNotice {
    NcTickOutcome outcome;
    struct UpdateNotice *next;
} UpdateNotice;

typedef struct UpdateBus {
    volatile LONG refs;
    SRWLOCK lock;
    HWND hwnd;
    UINT message;
    UpdateNotice *head;
    UpdateNotice *tail;
} UpdateBus;

typedef struct {
    UpdateBus *bus;
    char feed[2048];
    NcVersion current;
    NcUpdateSettingsSnapshot snapshot;
} TickWorkerContext;

struct NcUpdateService {
    NcUpdateHost host;
    UpdateBus *bus;
    bool busy;
    bool install_started;
    bool deferred_logged;
    bool has_pending;
    NcUpdateManifest pending_manifest;
    WStr pending_installer;
    uint64_t generation;
    bool has_active;
    uint64_t active_generation;
    char active_feed[2048];
    bool has_pending_request;
    uint64_t pending_request_generation;
    char pending_request_feed[2048];
};

static UpdateBus *bus_new(HWND hwnd, UINT message) {
    UpdateBus *bus = (UpdateBus *)calloc(1, sizeof(UpdateBus));
    if (!bus) {
        return NULL;
    }
    InitializeSRWLock(&bus->lock);
    bus->refs = 1;
    bus->hwnd = hwnd;
    bus->message = message;
    return bus;
}

static void bus_retain(UpdateBus *bus) {
    if (bus) {
        InterlockedIncrement(&bus->refs);
    }
}

static void bus_release(UpdateBus *bus) {
    UpdateNotice *node;
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
        UpdateNotice *next = node->next;
        nc_tick_outcome_free(&node->outcome);
        free(node);
        node = next;
    }
    free(bus);
}

static void bus_push(UpdateBus *bus, NcTickOutcome *outcome) {
    UpdateNotice *node;
    HWND hwnd;
    UINT message;
    if (!bus || !outcome) {
        return;
    }
    node = (UpdateNotice *)malloc(sizeof(UpdateNotice));
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
    message = bus->message;
    ReleaseSRWLockExclusive(&bus->lock);
    if (hwnd) {
        PostMessageW(hwnd, message, 0, 0);
    }
}

static bool bus_pop(UpdateBus *bus, NcTickOutcome *outcome) {
    UpdateNotice *node;
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

static unsigned __stdcall tick_worker(void *parameter) {
    TickWorkerContext *context = (TickWorkerContext *)parameter;
    NcTickOutcome outcome;
    nc_coordinator_tick(context->feed, &context->current, &context->snapshot, &outcome);
    bus_push(context->bus, &outcome);
    bus_release(context->bus);
    free(context->snapshot.last_attempted_version);
    free(context);
    return 0;
}

NcUpdateService *nc_update_service_new(HWND notify_hwnd, UINT notify_message,
                                       const NcUpdateHost *host) {
    NcUpdateService *service = (NcUpdateService *)calloc(1, sizeof(NcUpdateService));
    if (!service) {
        return NULL;
    }
    if (host) {
        service->host = *host;
    }
    wstr_init(&service->pending_installer);
    service->bus = bus_new(notify_hwnd, notify_message);
    if (!service->bus) {
        free(service);
        return NULL;
    }
    return service;
}

void nc_update_service_detach_window(NcUpdateService *service) {
    if (!service || !service->bus) {
        return;
    }
    AcquireSRWLockExclusive(&service->bus->lock);
    service->bus->hwnd = NULL;
    ReleaseSRWLockExclusive(&service->bus->lock);
}

void nc_update_service_free(NcUpdateService *service) {
    if (!service) {
        return;
    }
    nc_update_service_detach_window(service);
    bus_release(service->bus);
    service->bus = NULL;
    nc_update_manifest_free(&service->pending_manifest);
    wstr_free(&service->pending_installer);
    free(service);
}

void nc_update_service_on_settings_changed(NcUpdateService *service, NcSettings *settings) {
    if (!service) {
        return;
    }
    service->generation++;
    nc_update_manifest_free(&service->pending_manifest);
    wstr_clear(&service->pending_installer);
    service->has_pending = false;
    service->has_pending_request = false;
    service->deferred_logged = false;
    if (settings) {
        nc_update_service_maybe_tick(service, settings);
    }
}

void nc_update_service_try_install(NcUpdateService *service, NcSettings *settings) {
    const char *version_text;
    if (!service || !settings || service->install_started || !service->has_pending) {
        return;
    }
    if (!settings->auto_update || !settings->update_feed_url || !settings->update_feed_url[0] ||
        !service->has_pending_request ||
        service->pending_request_generation != service->generation ||
        !nc_ieq_ascii(service->pending_request_feed, settings->update_feed_url)) {
        nc_update_manifest_free(&service->pending_manifest);
        wstr_clear(&service->pending_installer);
        service->has_pending = false;
        service->has_pending_request = false;
        return;
    }
    if (service->host.is_idle && !service->host.is_idle(service->host.userdata)) {
        if (!service->deferred_logged) {
            service->deferred_logged = true;
            nc_update_log("当前弱网运行中或有窗口打开，暂不安装，等空闲后再继续。");
        }
        return;
    }
    service->install_started = true;
    service->deferred_logged = false;
    version_text = service->pending_manifest.version ? service->pending_manifest.version : "";
    {
        char message[512];
        snprintf(message, sizeof(message), "开始自更新到版本 %s。", version_text);
        nc_update_log(message);
    }
    if (service->host.stop_engine_sync) {
        service->host.stop_engine_sync(service->host.userdata);
    }
    settings->last_update_attempt_unix = nc_now_unix();
    free(settings->last_attempted_version);
    settings->last_attempted_version = nc_strdup(version_text);
    if (service->host.save_config) {
        service->host.save_config(service->host.userdata);
    }
    if (!nc_update_apply_self_update(wstr_cstr(&service->pending_installer))) {
        service->install_started = false;
        nc_update_log("启动自更新失败，继续使用当前版本。");
        return;
    }
    if (nc_update_is_dry_run()) {
        nc_update_log("DRY RUN：保持当前进程运行，不重启。");
    } else if (service->host.exit_for_update) {
        service->host.exit_for_update(service->host.userdata);
    }
}

void nc_update_service_maybe_tick(NcUpdateService *service, NcSettings *settings) {
    TickWorkerContext *context;
    HANDLE thread;
    if (!service || !service->bus || !settings) {
        return;
    }
    if (service->install_started) {
        return;
    }
    if (service->has_pending) {
        nc_update_service_try_install(service, settings);
        return;
    }
    if (service->busy) {
        return;
    }
    if (!settings->auto_update || !settings->update_feed_url || !settings->update_feed_url[0]) {
        return;
    }
    if (!nc_should_check(settings, nc_now_unix())) {
        return;
    }
    context = (TickWorkerContext *)calloc(1, sizeof(TickWorkerContext));
    if (!context) {
        return;
    }
    context->bus = service->bus;
    bus_retain(service->bus);
    snprintf(context->feed, sizeof(context->feed), "%s", settings->update_feed_url);
    context->current = nc_version_current();
    context->snapshot.last_update_attempt_unix = settings->last_update_attempt_unix;
    context->snapshot.last_attempted_version =
        nc_strdup(settings->last_attempted_version ? settings->last_attempted_version : "");
    service->busy = true;
    service->has_active = true;
    service->active_generation = service->generation;
    snprintf(service->active_feed, sizeof(service->active_feed), "%s", context->feed);
    thread = (HANDLE)_beginthreadex(NULL, 0, tick_worker, context, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        bus_release(service->bus);
        free(context->snapshot.last_attempted_version);
        free(context);
        service->busy = false;
        service->has_active = false;
    }
}

void nc_update_service_drain(NcUpdateService *service, NcSettings *settings) {
    NcTickOutcome outcome;
    if (!service || !service->bus || !settings) {
        return;
    }
    while (bus_pop(service->bus, &outcome)) {
        bool current;
        service->busy = false;
        current = service->has_active && service->active_generation == service->generation &&
                  nc_ieq_ascii(service->active_feed,
                               settings->update_feed_url ? settings->update_feed_url : "");
        service->has_active = false;
        if (!current || !settings->auto_update) {
            nc_update_log("忽略来自旧更新设置的检查结果。");
            nc_tick_outcome_free(&outcome);
            nc_update_service_maybe_tick(service, settings);
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
            settings->last_update_check_unix = outcome.now;
            if (service->host.save_config) {
                service->host.save_config(service->host.userdata);
            }
            nc_update_log("已是最新版本。");
            break;
        case NC_TICK_READY:
            settings->last_update_check_unix = outcome.now;
            nc_update_manifest_free(&service->pending_manifest);
            service->pending_manifest = outcome.manifest;
            memset(&outcome.manifest, 0, sizeof(outcome.manifest));
            wstr_free(&service->pending_installer);
            service->pending_installer = outcome.installer;
            wstr_init(&outcome.installer);
            service->has_pending = true;
            service->has_pending_request = true;
            service->pending_request_generation = service->generation;
            snprintf(service->pending_request_feed, sizeof(service->pending_request_feed), "%s",
                     settings->update_feed_url);
            if (service->host.save_config) {
                service->host.save_config(service->host.userdata);
            }
            nc_update_service_try_install(service, settings);
            break;
        case NC_TICK_DOWNLOAD_FAILED:
            settings->last_update_check_unix = outcome.now;
            settings->last_update_attempt_unix = outcome.now;
            free(settings->last_attempted_version);
            settings->last_attempted_version =
                nc_strdup(outcome.manifest.version ? outcome.manifest.version : "");
            if (service->host.save_config) {
                service->host.save_config(service->host.userdata);
            }
            break;
        default:
            break;
        }
        nc_tick_outcome_free(&outcome);
    }
}
