#include "update.h"

#include <bcrypt.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "neturl.h"
#include "selfupdate.h"

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

void nc_sha256_hex(const void *data, size_t length, char out[65]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    DWORD digest_size = 0;
    DWORD result_size = 0;
    size_t index;
    if (!out) {
        return;
    }
    out[0] = 0;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
        return;
    }
    if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&digest_size,
                                          sizeof(digest_size), &result_size, 0)) ||
        digest_size != sizeof(digest)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return;
    }
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return;
    }
    if (length > 0) {
        size_t offset = 0;
        while (offset < length) {
            ULONG chunk = (ULONG)((length - offset) > 0x40000000u ? 0x40000000u : (length - offset));
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data + offset, chunk, 0))) {
                BCryptDestroyHash(hash);
                BCryptCloseAlgorithmProvider(algorithm, 0);
                return;
            }
            offset += chunk;
        }
    }
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, (ULONG)sizeof(digest), 0))) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    for (index = 0; index < sizeof(digest); index++) {
        snprintf(out + index * 2, 3, "%02X", digest[index]);
    }
}

bool nc_sha256_file(const wchar_t *path, uint64_t *size, char out[65], NcErr *err) {
    HANDLE file;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    unsigned char buffer[64 * 1024];
    uint64_t total = 0;
    size_t index;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return nc_fail(err, "打开文件失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
    }
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
        CloseHandle(file);
        return nc_fail(err, "初始化 SHA256 失败。");
    }
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        return nc_fail(err, "初始化 SHA256 失败。");
    }
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) {
            DWORD code = GetLastError();
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            CloseHandle(file);
            return nc_fail(err, "读取文件失败（Win32 错误 %lu）。", (unsigned long)code);
        }
        if (read == 0) {
            break;
        }
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer, read, 0))) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            CloseHandle(file);
            return nc_fail(err, "计算 SHA256 失败。");
        }
        total += read;
    }
    CloseHandle(file);
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, (ULONG)sizeof(digest), 0))) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return nc_fail(err, "计算 SHA256 失败。");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    for (index = 0; index < sizeof(digest); index++) {
        snprintf(out + index * 2, 3, "%02X", digest[index]);
    }
    if (size) {
        *size = total;
    }
    return true;
}

bool nc_update_directory(wchar_t *out, size_t cap) {
    wchar_t base[32768];
    wchar_t directory[32768];
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!nc_env_absolute_dir(L"LOCALAPPDATA", base, sizeof(base) / sizeof(base[0]))) {
        if (!nc_known_folder(&FOLDERID_LocalAppData, base, sizeof(base) / sizeof(base[0]))) {
            return false;
        }
    }
    if (!nc_path_join(directory, sizeof(directory) / sizeof(directory[0]), base, L"NetConditioner")) {
        return false;
    }
    return nc_path_join(out, cap, directory, L"update");
}

static bool restrict_directory_to_user(const wchar_t *directory) {
    PSECURITY_DESCRIPTOR descriptor = NULL;
    BOOL result;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, NULL)) {
        return false;
    }
    result = SetFileSecurityW(directory,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              descriptor);
    LocalFree(descriptor);
    return result != 0;
}

bool nc_update_ensure_directory(wchar_t *out, size_t cap, NcErr *err) {
    wchar_t directory[32768];
    if (!nc_update_directory(directory, sizeof(directory) / sizeof(directory[0]))) {
        return nc_fail(err, "无法确定更新目录。");
    }
    if (!nc_make_dirs(directory)) {
        return nc_fail(err, "创建更新目录失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
    }
    restrict_directory_to_user(directory);
    if (out) {
        nc_copy_wide(out, cap, directory);
    }
    return true;
}

void nc_update_log(const char *message) {
    wchar_t directory[32768];
    wchar_t path[32768];
    char line[2048];
    char stamp[64];
    NcErr error;
    ULARGE_INTEGER size;
    nc_update_ensure_directory(directory, sizeof(directory) / sizeof(directory[0]), &error);
    if (!nc_path_join(path, sizeof(path) / sizeof(path[0]), directory, L"update.log")) {
        return;
    }
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        memset(&data, 0, sizeof(data));
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &data)) {
            size.LowPart = data.nFileSizeLow;
            size.HighPart = data.nFileSizeHigh;
            if (size.QuadPart > 512 * 1024) {
                wchar_t backup[32768];
                swprintf(backup, sizeof(backup) / sizeof(backup[0]), L"%ls.old", path);
                MoveFileExW(path, backup, MOVEFILE_REPLACE_EXISTING);
            }
        }
    }
    nc_format_local_iso_ms(stamp, sizeof(stamp));
    snprintf(line, sizeof(line), "[%s] %s\r\n", stamp, message ? message : "");
    nc_append_file(path, line, strlen(line), &error);
}

bool nc_update_is_dry_run(void) {
    char value[16];
    DWORD length = GetEnvironmentVariableA("NC_UPDATE_DRY_RUN", value, sizeof(value));
    return length == 1 && value[0] == '1';
}

void nc_update_payload_path(const char *version, wchar_t *out, size_t cap) {
    char normalized[32];
    wchar_t wide_name[128];
    wchar_t directory[32768];
    if (!out || cap == 0) {
        return;
    }
    out[0] = 0;
    if (!nc_version_normalize(version, normalized, sizeof(normalized))) {
        snprintf(normalized, sizeof(normalized), "unknown");
    }
    swprintf(wide_name, sizeof(wide_name) / sizeof(wide_name[0]),
             L"NetConditioner-%hs-win-x64.exe", normalized);
    if (!nc_update_directory(directory, sizeof(directory) / sizeof(directory[0]))) {
        return;
    }
    nc_path_join(out, cap, directory, wide_name);
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
        int32_t raw = 0;
        json_get_i32(root, "schemaVersion", &raw);
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
    wchar_t parent[32768];
    wchar_t staging[32768];
    HANDLE handle = INVALID_HANDLE_VALUE;
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
    parent[0] = 0;
    nc_path_parent(destination, parent, sizeof(parent) / sizeof(parent[0]));
    if (parent[0]) {
        nc_make_dirs(parent);
    }
    if (!nc_http_get(manifest->installer.url, !loopback, NC_DOWNLOAD_TIMEOUT_MS,
                     (size_t)NC_MAX_INSTALLER_BYTES, &response, &error)) {
        return nc_fail(err, "%s", error.msg);
    }
    if (response.status < 200 || response.status >= 300) {
        int status = response.status;
        nc_http_response_free(&response);
        return nc_fail(err, "服务器返回 HTTP %d。", status);
    }
    if (response.body_size == 0) {
        nc_http_response_free(&response);
        return nc_fail(err, "更新包为空。");
    }
    if (response.body_size != expected_size) {
        size_t actual = response.body_size;
        nc_http_response_free(&response);
        return nc_fail(err, "文件大小 %llu 与清单 %llu 不一致。", (unsigned long long)actual,
                       (unsigned long long)expected_size);
    }
    nc_sha256_hex(response.body, response.body_size, digest);
    if (!nc_ieq_ascii(digest, expected)) {
        nc_http_response_free(&response);
        return nc_fail(err, "SHA256 不匹配。");
    }
    if (!nc_create_temp_file(parent[0] ? parent : L".", L"nc-download", response.body,
                             response.body_size, staging, sizeof(staging) / sizeof(staging[0]),
                             &handle, &error)) {
        nc_http_response_free(&response);
        return nc_fail(err, "%s", error.msg);
    }
    CloseHandle(handle);
    nc_http_response_free(&response);
    if (!nc_replace_file(staging, destination, &error)) {
        nc_remove_file(staging);
        return nc_fail(err, "%s", error.msg);
    }
    {
        uint64_t final_size = 0;
        char final_hash[65];
        if (!nc_sha256_file(destination, &final_size, final_hash, &error) ||
            final_size != expected_size || !nc_ieq_ascii(final_hash, expected)) {
            nc_remove_file(destination);
            return nc_fail(err, "最终更新包复核失败。");
        }
    }
    nc_selfupdate_remove_zone_identifier(destination);
    {
        char message[1200];
        snprintf(message, sizeof(message), "下载完成并校验通过：%llu 字节",
                 (unsigned long long)expected_size);
        nc_update_log(message);
    }
    return true;
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

void nc_update_cleanup_old_files(void) {
    wchar_t directory[32768];
    wchar_t pattern[32768];
    WIN32_FIND_DATAW data;
    HANDLE find;
    FILETIME now;
    ULARGE_INTEGER cutoff;
    GetSystemTimeAsFileTime(&now);
    cutoff.LowPart = now.dwLowDateTime;
    cutoff.HighPart = now.dwHighDateTime;
    cutoff.QuadPart = cutoff.QuadPart > 7ull * 24ull * 3600ull * 10000000ull
                          ? cutoff.QuadPart - 7ull * 24ull * 3600ull * 10000000ull
                          : 0;
    if (!nc_update_directory(directory, sizeof(directory) / sizeof(directory[0]))) {
        return;
    }
    if (!nc_path_join(pattern, sizeof(pattern) / sizeof(pattern[0]), directory, L"*")) {
        return;
    }
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        ULARGE_INTEGER written;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        written.LowPart = data.ftLastWriteTime.dwLowDateTime;
        written.HighPart = data.ftLastWriteTime.dwHighDateTime;
        if (written.QuadPart < cutoff.QuadPart) {
            wchar_t path[32768];
            if (nc_path_join(path, sizeof(path) / sizeof(path[0]), directory, data.cFileName)) {
                DeleteFileW(path);
            }
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}
