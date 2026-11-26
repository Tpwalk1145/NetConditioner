#include "store.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    READ_OK = 0,
    READ_NOT_FOUND,
    READ_TOO_LARGE,
    READ_ERROR
} ReadStatus;

static void warn_set(char *buffer, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (!buffer || cap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buffer, cap, fmt, ap);
    va_end(ap);
    buffer[cap - 1] = 0;
}

static void warn_append(char *buffer, size_t cap, const char *fmt, ...) {
    size_t used;
    va_list ap;
    if (!buffer || cap == 0) {
        return;
    }
    used = strlen(buffer);
    if (used + 1 >= cap) {
        return;
    }
    {
        size_t offset = used;
        if (used > 0) {
            int written = snprintf(buffer + used, cap - used, "\r\n");
            if (written > 0) {
                offset += (size_t)written;
            }
        }
        if (offset < cap) {
            va_start(ap, fmt);
            vsnprintf(buffer + offset, cap - offset, fmt, ap);
            va_end(ap);
            buffer[cap - 1] = 0;
        }
    }
}

bool nc_store_default_path(wchar_t *out, size_t cap) {
    wchar_t base[32768];
    wchar_t directory[32768];
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!nc_env_absolute_dir(L"APPDATA", base, sizeof(base) / sizeof(base[0]))) {
        if (!nc_known_folder(&FOLDERID_RoamingAppData, base, sizeof(base) / sizeof(base[0]))) {
            return false;
        }
    }
    if (!nc_path_join(directory, sizeof(directory) / sizeof(directory[0]), base, L"NetConditioner")) {
        return false;
    }
    return nc_path_join(out, cap, directory, L"config.json");
}

void nc_store_init(NcStore *store) {
    if (!store) {
        return;
    }
    store->path[0] = 0;
    nc_store_default_path(store->path, sizeof(store->path) / sizeof(store->path[0]));
}

static HANDLE acquire_lock(const wchar_t *path) {
    wchar_t name[128];
    HANDLE handle;
    swprintf(name, sizeof(name) / sizeof(name[0]), L"Local\\NetConditioner-config-%016llx",
             (unsigned long long)nc_fnv1a_wide(path));
    handle = CreateMutexW(NULL, FALSE, name);
    if (!handle) {
        return NULL;
    }
    if (WaitForSingleObject(handle, 10000) != WAIT_OBJECT_0 &&
        GetLastError() != WAIT_ABANDONED) {
        CloseHandle(handle);
        return NULL;
    }
    return handle;
}

static void release_lock(HANDLE handle) {
    if (handle) {
        ReleaseMutex(handle);
        CloseHandle(handle);
    }
}

static bool valid_utf8(const void *data, size_t size) {
    if (!data || size == 0) {
        return true;
    }
    if (size > 0x7FFFFFFF) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)data, (int)size,
                               NULL, 0) > 0;
}

static ReadStatus read_bounded(const wchar_t *path, size_t max_bytes, void **data, size_t *size,
                               NcErr *err) {
    HANDLE file;
    DWORD code;
    LARGE_INTEGER length;
    void *buffer;
    size_t offset = 0;
    *data = NULL;
    *size = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        code = GetLastError();
        nc_err_set(err, "打开配置文件失败（Win32 错误 %lu）。", (unsigned long)code);
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return READ_NOT_FOUND;
        }
        return READ_ERROR;
    }
    if (!GetFileSizeEx(file, &length)) {
        code = GetLastError();
        CloseHandle(file);
        nc_err_set(err, "读取配置文件大小失败（Win32 错误 %lu）。", (unsigned long)code);
        return READ_ERROR;
    }
    if (length.QuadPart < 0 || (uint64_t)length.QuadPart > (uint64_t)max_bytes) {
        CloseHandle(file);
        return READ_TOO_LARGE;
    }
    buffer = malloc(length.QuadPart > 0 ? (size_t)length.QuadPart : 1);
    if (!buffer) {
        CloseHandle(file);
        nc_err_set(err, "内存不足。");
        return READ_ERROR;
    }
    while (offset < (size_t)length.QuadPart) {
        DWORD request = (DWORD)(((size_t)length.QuadPart - offset) > 0x100000u
                                    ? 0x100000u
                                    : ((size_t)length.QuadPart - offset));
        DWORD read = 0;
        if (!ReadFile(file, (unsigned char *)buffer + offset, request, &read, NULL) || read == 0) {
            code = GetLastError();
            free(buffer);
            CloseHandle(file);
            nc_err_set(err, "读取配置文件失败（Win32 错误 %lu）。", (unsigned long)code);
            return READ_ERROR;
        }
        offset += read;
    }
    CloseHandle(file);
    *data = buffer;
    *size = (size_t)length.QuadPart;
    return READ_OK;
}

static bool schema_supported(const JsonValue *root) {
    JsonValue *version;
    JsonValue *schema;
    if (!root || root->type != JSON_OBJECT) {
        return false;
    }
    version = json_object_get(root, "version");
    if (!version || version->type != JSON_NUMBER || !version->u.number.is_int ||
        version->u.number.integer != 1) {
        return false;
    }
    schema = json_object_get(root, "schemaVersion");
    if (schema) {
        if (schema->type != JSON_NUMBER || !schema->u.number.is_int || schema->u.number.integer != 1) {
            return false;
        }
    }
    return true;
}

static bool backup_file(const wchar_t *path, const wchar_t *directory, wchar_t *backup_path,
                        size_t cap, NcErr *err) {
    HANDLE source;
    HANDLE target = INVALID_HANDLE_VALUE;
    unsigned char buffer[64 * 1024];
    bool ok = false;
    backup_path[0] = 0;
    if (!nc_create_temp_file(directory, L"nc-config-backup", NULL, 0, backup_path, cap, &target, err)) {
        return false;
    }
    source = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (source == INVALID_HANDLE_VALUE) {
        nc_fail(err, "打开原配置文件失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        CloseHandle(target);
        nc_remove_file(backup_path);
        return false;
    }
    for (;;) {
        DWORD read = 0;
        DWORD written = 0;
        if (!ReadFile(source, buffer, sizeof(buffer), &read, NULL)) {
            nc_fail(err, "读取原配置文件失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
            break;
        }
        if (read == 0) {
            ok = FlushFileBuffers(target) != 0;
            if (!ok) {
                nc_fail(err, "同步备份文件失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
            }
            break;
        }
        if (!WriteFile(target, buffer, read, &written, NULL) || written != read) {
            nc_fail(err, "写入备份文件失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
            break;
        }
    }
    CloseHandle(source);
    CloseHandle(target);
    if (!ok) {
        nc_remove_file(backup_path);
        backup_path[0] = 0;
    }
    return ok;
}

static bool save_locked(const wchar_t *path, const NcConfig *config, NcErr *err) {
    wchar_t parent[32768];
    NcErr inner;
    JsonValue *document = NULL;
    JsonValue *updated = NULL;
    Str json;
    void *existing = NULL;
    size_t existing_size = 0;
    NcErr read_error;
    bool needs_write = true;
    bool ok = false;
    if (!path || !config) {
        return nc_fail(err, "配置保存参数无效。");
    }
    parent[0] = 0;
    nc_path_parent(path, parent, sizeof(parent) / sizeof(parent[0]));
    if (parent[0] == 0) {
        nc_copy_wide(parent, sizeof(parent) / sizeof(parent[0]), L".");
    }
    nc_make_dirs(parent);
    if (read_bounded(path, NC_CONFIG_MAX_BYTES, &existing, &existing_size, &read_error) == READ_OK) {
        if (valid_utf8(existing, existing_size)) {
            document = json_parse((const char *)existing, existing_size, &inner);
        }
    }
    if (!document) {
        document = json_object();
    }
    updated = nc_config_to_json(config);
    if (!document || !updated) {
        json_free(document);
        json_free(updated);
        free(existing);
        return nc_fail(err, "内存不足。");
    }
    json_merge_known_fields(document, updated);
    json_free(updated);
    str_init(&json);
    if (!json_serialize(document, &json) || !json.data) {
        json_free(document);
        free(existing);
        str_free(&json);
        return nc_fail(err, "序列化配置失败。");
    }
    json_free(document);
    if (existing && existing_size == json.len && memcmp(existing, json.data, json.len) == 0) {
        needs_write = false;
    }
    free(existing);
    if (needs_write) {
        wchar_t temp[32768];
        HANDLE handle = INVALID_HANDLE_VALUE;
        if (!nc_create_temp_file(parent, L"nc-config", json.data, json.len, temp,
                                 sizeof(temp) / sizeof(temp[0]), &handle, &inner)) {
            nc_err_set(err, "%s", inner.msg);
        } else {
            CloseHandle(handle);
            if (!nc_replace_file(temp, path, &inner)) {
                nc_remove_file(temp);
                nc_err_set(err, "%s", inner.msg);
            } else {
                ok = true;
            }
        }
    } else {
        ok = true;
    }
    str_free(&json);
    return ok ? true : false;
}

static void recover_locked(const wchar_t *path, const char *reason, NcConfig *out, char *warning,
                           size_t warning_cap) {
    wchar_t parent[32768];
    wchar_t backup[32768];
    NcErr backup_error;
    NcConfig fresh;
    nc_config_fresh(&fresh);
    {
        size_t removed = 0;
        nc_config_sanitize(&fresh, nc_now_unix(), &removed);
    }
    parent[0] = 0;
    nc_path_parent(path, parent, sizeof(parent) / sizeof(parent[0]));
    if (parent[0] == 0) {
        nc_copy_wide(parent, sizeof(parent) / sizeof(parent[0]), L".");
    }
    if (!backup_file(path, parent, backup, sizeof(backup) / sizeof(backup[0]), &backup_error)) {
        warn_set(warning, warning_cap,
                 "配置文件读取失败，且备份失败；保留原文件未覆盖。\r\n%s\r\n备份失败：%s",
                 reason, backup_error.msg);
    } else {
        NcErr save_error;
        warn_set(warning, warning_cap, "配置文件读取失败，已恢复默认配置。\r\n%s", reason);
        if (!save_locked(path, &fresh, &save_error)) {
            warn_append(warning, warning_cap, "保存配置失败：%s", save_error.msg);
        }
    }
    *out = fresh;
}

bool nc_store_load(NcStore *store, NcConfig *out, char *warning, size_t warning_cap) {
    HANDLE lock;
    void *bytes = NULL;
    size_t size = 0;
    NcErr error;
    ReadStatus status;
    if (warning && warning_cap) {
        warning[0] = 0;
    }
    if (!store || !out || store->path[0] == 0) {
        return false;
    }
    lock = acquire_lock(store->path);
    status = read_bounded(store->path, NC_CONFIG_MAX_BYTES, &bytes, &size, &error);
    if (status == READ_NOT_FOUND) {
        size_t removed = 0;
        nc_config_fresh(out);
        nc_config_sanitize(out, nc_now_unix(), &removed);
        if (!save_locked(store->path, out, &error)) {
            warn_set(warning, warning_cap, "保存配置失败：%s", error.msg);
        }
        release_lock(lock);
        return true;
    }
    if (status == READ_TOO_LARGE) {
        size_t removed = 0;
        nc_config_fresh(out);
        nc_config_sanitize(out, nc_now_unix(), &removed);
        warn_set(warning, warning_cap, "配置文件超过 1 MiB 限制，未修改原文件。");
        release_lock(lock);
        return true;
    }
    if (status == READ_ERROR) {
        recover_locked(store->path, "配置文件读取失败。", out, warning, warning_cap);
        release_lock(lock);
        return true;
    }
    if (!valid_utf8(bytes, size)) {
        free(bytes);
        recover_locked(store->path, "配置文件不是 UTF-8。", out, warning, warning_cap);
        release_lock(lock);
        return true;
    }
    {
        JsonValue *parsed = json_parse((const char *)bytes, size, &error);
        free(bytes);
        if (!parsed) {
            recover_locked(store->path, "配置文件不是有效的 JSON。", out, warning, warning_cap);
            release_lock(lock);
            return true;
        }
        if (!schema_supported(parsed)) {
            size_t removed = 0;
            nc_config_fresh(out);
            nc_config_sanitize(out, nc_now_unix(), &removed);
            warn_set(warning, warning_cap, "配置文件 schemaVersion 不受支持，未修改原文件。");
            json_free(parsed);
            release_lock(lock);
            return true;
        }
        {
            NcConfig config;
            size_t removed = 0;
            JsonValue *updated;
            nc_config_init(&config);
            nc_config_from_json(parsed, &config);
            nc_config_sanitize(&config, nc_now_unix(), &removed);
            if (removed > 0) {
                warn_set(warning, warning_cap, "已忽略 %zu 个损坏的配置项。", removed);
            }
            updated = nc_config_to_json(&config);
            if (!updated || !json_known_fields_equal(parsed, updated)) {
                NcErr save_error;
                if (!save_locked(store->path, &config, &save_error)) {
                    warn_append(warning, warning_cap, "保存配置失败：%s", save_error.msg);
                }
            }
            json_free(updated);
            json_free(parsed);
            *out = config;
        }
    }
    release_lock(lock);
    return true;
}

bool nc_store_save(NcStore *store, const NcConfig *config, NcErr *err) {
    HANDLE lock;
    bool ok;
    if (!store || !config || store->path[0] == 0) {
        return nc_fail(err, "配置保存路径无效。");
    }
    lock = acquire_lock(store->path);
    ok = save_locked(store->path, config, err);
    release_lock(lock);
    return ok;
}
