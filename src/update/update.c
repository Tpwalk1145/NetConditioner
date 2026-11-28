#include "update/update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/json.h"
#include "core/neturl.h"
#include "core/str.h"
#include "update/http.h"
#include "update/selfupdate.h"

#define NC_MAX_MANIFEST_BYTES (64u * 1024u)
#define NC_MAX_INSTALLER_BYTES (300ull * 1024ull * 1024ull)
#define NC_CHECK_TIMEOUT_MS 10000u
#define NC_DOWNLOAD_TIMEOUT_MS 300000u

void nc_update_manifest_free(NcUpdateManifest *manifest) {
    if (!manifest) {
        return;
    }
    free(manifest->version);
    free(manifest->published_at);
    free(manifest->notes);
    free(manifest->page_url);
    free(manifest->installer.url);
    free(manifest->installer.sha256);
    memset(manifest, 0, sizeof(*manifest));
}

static char *dup_json_string(const JsonValue *object, const char *key) {
    JsonValue *value = json_object_get(object, key);
    return value && value->type == JSON_STRING ? nc_strdup(value->u.string) : NULL;
}

bool nc_update_parse_manifest(const char *json, size_t length, NcUpdateManifest *out,
                              NcVersion *version, NcErr *err) {
    NcErr parse_error;
    JsonValue *root;
    JsonValue *installer;
    int32_t schema_version = 0;
    if (!out || !version) {
        return nc_fail(err, "内部错误。");
    }
    memset(out, 0, sizeof(*out));
    root = json_parse(json, length, &parse_error);
    if (!root) {
        return nc_fail(err, "更新信息不是有效的 JSON。");
    }
    if (!json_get_i32(root, "schemaVersion", &schema_version) || schema_version != 2) {
        json_free(root);
        return nc_fail(err, "不支持的更新信息版本：%d。", (int)schema_version);
    }
    out->schema_version = schema_version;
    out->version = dup_json_string(root, "version");
    out->published_at = dup_json_string(root, "publishedAt");
    out->notes = dup_json_string(root, "notes");
    out->page_url = dup_json_string(root, "pageUrl");
    if (!out->version || !nc_version_parse(out->version, version)) {
        json_free(root);
        nc_update_manifest_free(out);
        return nc_fail(err, "更新信息中的版本号无效。");
    }
    installer = json_get_object(root, "installer");
    if (!installer) {
        json_free(root);
        nc_update_manifest_free(out);
        return nc_fail(err, "更新信息缺少更新包信息。");
    }
    out->installer.url = dup_json_string(installer, "url");
    out->installer.sha256 = dup_json_string(installer, "sha256");
    {
        JsonValue *size = json_object_get(installer, "size");
        if (!size || size->type != JSON_NUMBER || !size->u.number.is_int) {
            out->installer.size = 0;
        } else {
            out->installer.size = size->u.number.integer;
        }
    }
    if (!out->installer.url) {
        json_free(root);
        nc_update_manifest_free(out);
        return nc_fail(err, "更新包地址无效。");
    }
    {
        NcErr url_error;
        if (!nc_url_validate_installer(out->installer.url, &url_error)) {
            json_free(root);
            nc_update_manifest_free(out);
            return nc_fail(err, "%s", url_error.msg);
        }
    }
    if (!out->installer.sha256) {
        json_free(root);
        nc_update_manifest_free(out);
        return nc_fail(err, "更新信息中的 SHA256 无效。");
    }
    {
        char normalized[65];
        if (!nc_neturl_normalize_sha256(out->installer.sha256, normalized)) {
            json_free(root);
            nc_update_manifest_free(out);
            return nc_fail(err, "更新信息中的 SHA256 无效。");
        }
        free(out->installer.sha256);
        out->installer.sha256 = nc_strdup(normalized);
    }
    if (out->installer.size <= 0 || (uint64_t)out->installer.size > NC_MAX_INSTALLER_BYTES) {
        json_free(root);
        nc_update_manifest_free(out);
        return nc_fail(err, "更新包大小超出允许范围。");
    }
    if (out->notes && nc_config_utf8_char_count(out->notes) > 4000) {
        json_free(root);
        nc_update_manifest_free(out);
        return nc_fail(err, "更新说明过长。");
    }
    if (out->page_url && out->page_url[0]) {
        NcUrl page;
        NcErr page_error;
        if (!nc_url_parse(out->page_url, "更新页面地址", &page, &page_error)) {
            json_free(root);
            nc_update_manifest_free(out);
            return nc_fail(err, "更新页面地址无效。");
        }
    }
    out->has_installer = true;
    json_free(root);
    return true;
}

void nc_update_check_result_free(NcUpdateCheckResult *result) {
    if (!result) {
        return;
    }
    free(result->error);
    nc_update_manifest_free(&result->manifest);
    memset(result, 0, sizeof(*result));
}

static NcUpdateCheckResult failed_result(const char *message) {
    NcUpdateCheckResult result;
    memset(&result, 0, sizeof(result));
    result.kind = NC_UPDATE_CHECK_FAILED;
    result.error = nc_strdup(message ? message : "检查更新失败。");
    return result;
}

NcUpdateCheckResult nc_update_check(const char *feed_url, const NcVersion *current) {
    NcUpdateCheckResult result;
    NcErr error;
    NcUrl parsed;
    NcHttpResponse response;
    char *text;
    NcVersion candidate;
    memset(&result, 0, sizeof(result));
    if (!nc_url_validate_feed(feed_url, &error)) {
        result.kind = NC_UPDATE_CHECK_NOT_CONFIGURED;
        result.error = nc_strdup(error.msg);
        return result;
    }
    if (!nc_url_parse(feed_url, "更新地址", &parsed, &error)) {
        result.kind = NC_UPDATE_CHECK_FAILED;
        result.error = nc_strdup(error.msg);
        return result;
    }
    {
        bool loopback = nc_ieq_ascii(parsed.scheme, "http");
        if (!nc_http_get(feed_url, !loopback, NC_CHECK_TIMEOUT_MS, NC_MAX_MANIFEST_BYTES, &response,
                         &error)) {
            char message[1200];
            snprintf(message, sizeof(message), "检查更新失败：%s", error.msg);
            result.kind = NC_UPDATE_CHECK_FAILED;
            result.error = nc_strdup(message);
            return result;
        }
    }
    if (response.status < 200 || response.status >= 300) {
        char message[128];
        snprintf(message, sizeof(message), "更新服务器返回 HTTP %d。", response.status);
        nc_http_response_free(&response);
        result.kind = NC_UPDATE_CHECK_FAILED;
        result.error = nc_strdup(message);
        return result;
    }
    text = (char *)malloc(response.body_size + 1);
    if (!text) {
        nc_http_response_free(&response);
        return failed_result("内存不足。");
    }
    if (response.body_size > 0) {
        memcpy(text, response.body, response.body_size);
    }
    text[response.body_size] = 0;
    nc_http_response_free(&response);
    {
        NcUpdateManifest manifest;
        if (!nc_update_parse_manifest(text, strlen(text), &manifest, &candidate, &error)) {
            free(text);
            return failed_result(error.msg);
        }
        free(text);
        result.manifest = manifest;
        result.version = candidate;
        result.kind = nc_version_is_newer(current, &candidate) ? NC_UPDATE_CHECK_AVAILABLE
                                                               : NC_UPDATE_CHECK_UP_TO_DATE;
    }
    return result;
}

bool nc_update_download(const NcUpdateManifest *manifest, const wchar_t *destination, NcErr *err) {
    NcHttpResponse response;
    NcErr error;
    NcUrl url;
    bool loopback;
    char expected[65];
    uint64_t expected_size;
    char digest[65];
    WStr parent;
    WStr staging;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool ok = false;
    if (!manifest || !manifest->has_installer || !manifest->installer.url ||
        !manifest->installer.sha256 || !destination) {
        return nc_fail(err, "更新包信息无效。");
    }
    if (!nc_neturl_normalize_sha256(manifest->installer.sha256, expected)) {
        return nc_fail(err, "SHA256 无效。");
    }
    if (manifest->installer.size <= 0 || (uint64_t)manifest->installer.size > NC_MAX_INSTALLER_BYTES) {
        return nc_fail(err, "更新包大小超出允许范围。");
    }
    expected_size = (uint64_t)manifest->installer.size;
    if (!nc_url_parse(manifest->installer.url, "更新包地址", &url, &error)) {
        return nc_fail(err, "%s", error.msg);
    }
    loopback = nc_ieq_ascii(url.scheme, "http");
    wstr_init(&parent);
    wstr_init(&staging);
    if (nc_path_parent_w(destination, &parent) && !wstr_empty(&parent)) {
        nc_make_dirs(wstr_cstr(&parent));
    }
    if (!nc_http_get(manifest->installer.url, !loopback, NC_DOWNLOAD_TIMEOUT_MS,
                     (size_t)NC_MAX_INSTALLER_BYTES, &response, &error)) {
        nc_err_set(err, "%s", error.msg);
        goto done;
    }
    if (response.status < 200 || response.status >= 300) {
        int status = response.status;
        nc_http_response_free(&response);
        nc_err_set(err, "服务器返回 HTTP %d。", status);
        goto done;
    }
    if (response.body_size == 0) {
        nc_http_response_free(&response);
        nc_err_set(err, "更新包为空。");
        goto done;
    }
    if (response.body_size != expected_size) {
        size_t actual = response.body_size;
        nc_http_response_free(&response);
        nc_err_set(err, "文件大小 %llu 与清单 %llu 不一致。", (unsigned long long)actual,
                   (unsigned long long)expected_size);
        goto done;
    }
    nc_sha256_hex(response.body, response.body_size, digest);
    if (!nc_ieq_ascii(digest, expected)) {
        nc_http_response_free(&response);
        nc_err_set(err, "SHA256 不匹配。");
        goto done;
    }
    if (!nc_create_temp_file_w(wstr_empty(&parent) ? L"." : wstr_cstr(&parent), L"nc-download",
                               response.body, response.body_size, &staging, &handle, &error)) {
        nc_http_response_free(&response);
        nc_err_set(err, "%s", error.msg);
        goto done;
    }
    CloseHandle(handle);
    nc_http_response_free(&response);
    if (!nc_replace_file(wstr_cstr(&staging), destination, &error)) {
        nc_remove_file(wstr_cstr(&staging));
        nc_err_set(err, "%s", error.msg);
        goto done;
    }
    {
        uint64_t final_size = 0;
        char final_hash[65];
        if (!nc_sha256_file(destination, &final_size, final_hash, &error) ||
            final_size != expected_size || !nc_ieq_ascii(final_hash, expected)) {
            nc_remove_file(destination);
            nc_err_set(err, "最终更新包复核失败。");
            goto done;
        }
    }
    nc_selfupdate_remove_zone_identifier(destination);
    {
        char message[1200];
        snprintf(message, sizeof(message), "下载完成并校验通过：%llu 字节",
                 (unsigned long long)expected_size);
        nc_update_log(message);
    }
    ok = true;
done:
    wstr_free(&staging);
    wstr_free(&parent);
    return ok;
}

bool nc_update_apply_self_update(const wchar_t *new_file) {
    NcErr error;
    if (nc_update_is_dry_run()) {
        nc_update_log("DRY RUN：跳过实际更新。");
        {
            char message[1200];
            snprintf(message, sizeof(message), "DRY RUN 更新包：%ls", new_file ? new_file : L"");
            nc_update_log(message);
        }
        return true;
    }
    if (!nc_file_exists(new_file)) {
        nc_update_log("启动自更新失败：更新包不存在。");
        return false;
    }
    if (!nc_selfupdate_spawn_helper(new_file, &error)) {
        char message[1200];
        snprintf(message, sizeof(message), "启动自更新失败：%s", error.msg);
        nc_update_log(message);
        return false;
    }
    nc_update_log("已启动自更新。");
    return true;
}
