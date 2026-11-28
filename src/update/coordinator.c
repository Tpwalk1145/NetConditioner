#include "update/coordinator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/neturl.h"
#include "core/time.h"

void nc_tick_outcome_free(NcTickOutcome *outcome) {
    if (!outcome) {
        return;
    }
    free(outcome->error);
    nc_update_manifest_free(&outcome->manifest);
    wstr_free(&outcome->installer);
    memset(outcome, 0, sizeof(*outcome));
}

bool nc_should_check(const NcSettings *settings, int64_t now) {
    if (!settings) {
        return false;
    }
    if (settings->last_update_check_unix <= 0) {
        return true;
    }
    if (settings->last_update_check_unix > now) {
        return true;
    }
    return now - settings->last_update_check_unix >= 24 * 60 * 60;
}

static bool retry_blocked(const NcUpdateSettingsSnapshot *snapshot, const char *version, int64_t now) {
    NcVersion previous;
    NcVersion candidate;
    if (!snapshot || !version) {
        return false;
    }
    if (!nc_version_parse(snapshot->last_attempted_version ? snapshot->last_attempted_version : "",
                          &previous) ||
        !nc_version_parse(version, &candidate)) {
        return false;
    }
    if (nc_version_compare(&previous, &candidate) != 0) {
        return false;
    }
    if (snapshot->last_update_attempt_unix <= 0) {
        return false;
    }
    return now - snapshot->last_update_attempt_unix < 24 * 60 * 60;
}

void nc_coordinator_tick(const char *feed_url, const NcVersion *current,
                         const NcUpdateSettingsSnapshot *snapshot, NcTickOutcome *outcome) {
    NcUpdateCheckResult check;
    int64_t now;
    char redacted[2048];
    if (!outcome) {
        return;
    }
    memset(outcome, 0, sizeof(*outcome));
    wstr_init(&outcome->installer);
    nc_url_redact(feed_url, redacted, sizeof(redacted));
    {
        char message[2200];
        snprintf(message, sizeof(message), "开始检查更新：%s", redacted);
        nc_update_log(message);
    }
    now = nc_now_unix();
    outcome->now = now;
    check = nc_update_check(feed_url, current);
    switch (check.kind) {
    case NC_UPDATE_CHECK_NOT_CONFIGURED:
    case NC_UPDATE_CHECK_FAILED:
        outcome->kind = NC_TICK_CHECK_FAILED;
        outcome->error = nc_strdup(check.error ? check.error : "检查更新失败。");
        break;
    case NC_UPDATE_CHECK_UP_TO_DATE:
        outcome->kind = NC_TICK_UP_TO_DATE;
        break;
    case NC_UPDATE_CHECK_AVAILABLE: {
        const char *version_text = check.manifest.version ? check.manifest.version : "";
        if (retry_blocked(snapshot, version_text, now)) {
            char message[256];
            snprintf(message, sizeof(message), "版本 %s 最近尝试升级失败，24 小时后再试。",
                     version_text);
            nc_update_log(message);
            outcome->kind = NC_TICK_UP_TO_DATE;
            break;
        }
        {
            char message[256];
            snprintf(message, sizeof(message), "发现新版本 %s，开始下载。", version_text);
            nc_update_log(message);
        }
        nc_update_payload_path_w(version_text, &outcome->installer);
        {
            NcErr error;
            if (!nc_update_download(&check.manifest, wstr_cstr(&outcome->installer), &error)) {
                char message[1400];
                snprintf(message, sizeof(message), "下载更新包失败：%s", error.msg);
                nc_update_log(message);
                outcome->kind = NC_TICK_DOWNLOAD_FAILED;
                outcome->manifest = check.manifest;
                memset(&check.manifest, 0, sizeof(check.manifest));
                break;
            }
        }
        {
            char message[256];
            snprintf(message, sizeof(message), "版本 %s 已下载并校验，等待空闲时安装。", version_text);
            nc_update_log(message);
        }
        outcome->kind = NC_TICK_READY;
        outcome->manifest = check.manifest;
        memset(&check.manifest, 0, sizeof(check.manifest));
        break;
    }
    default:
        outcome->kind = NC_TICK_CHECK_FAILED;
        outcome->error = nc_strdup("检查更新失败。");
        break;
    }
    free(check.error);
    nc_update_manifest_free(&check.manifest);
}
