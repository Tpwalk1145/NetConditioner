#include "update/update_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <knownfolders.h>
#include <sddl.h>
#include <stdio.h>
#include <string.h>

#include "core/fs.h"
#include "core/time.h"
#include "core/version.h"

bool nc_update_directory_w(WStr *out) {
    WStr base;
    WStr directory;
    bool ok = false;
    if (!out) {
        return false;
    }
    wstr_clear(out);
    wstr_init(&base);
    wstr_init(&directory);
    if (nc_env_absolute_dir_w(L"LOCALAPPDATA", &base) ||
        nc_known_folder_w(&FOLDERID_LocalAppData, &base)) {
        if (nc_path_join_w(&directory, wstr_cstr(&base), L"NetConditioner")) {
            ok = nc_path_join_w(out, wstr_cstr(&directory), L"update");
        }
    }
    wstr_free(&directory);
    wstr_free(&base);
    return ok;
}

bool nc_update_directory(wchar_t *out, size_t cap) {
    WStr dir;
    bool ok;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    wstr_init(&dir);
    ok = nc_update_directory_w(&dir) && nc_copy_wide(out, cap, wstr_cstr(&dir));
    wstr_free(&dir);
    return ok;
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

bool nc_update_ensure_directory_w(WStr *out, NcErr *err) {
    WStr directory;
    wstr_init(&directory);
    if (!nc_update_directory_w(&directory)) {
        wstr_free(&directory);
        return nc_fail(err, "无法确定更新目录。");
    }
    if (!nc_make_dirs(wstr_cstr(&directory))) {
        DWORD code = GetLastError();
        wstr_free(&directory);
        return nc_fail(err, "创建更新目录失败（Win32 错误 %lu）。", (unsigned long)code);
    }
    restrict_directory_to_user(wstr_cstr(&directory));
    if (out) {
        wstr_free(out);
        *out = directory;
    } else {
        wstr_free(&directory);
    }
    return true;
}

bool nc_update_ensure_directory(wchar_t *out, size_t cap, NcErr *err) {
    WStr directory;
    bool ok;
    wstr_init(&directory);
    if (!nc_update_ensure_directory_w(&directory, err)) {
        wstr_free(&directory);
        return false;
    }
    ok = !out || nc_copy_wide(out, cap, wstr_cstr(&directory));
    wstr_free(&directory);
    return ok;
}

void nc_update_log(const char *message) {
    WStr directory;
    WStr path;
    char line[2048];
    char stamp[64];
    NcErr error;
    WIN32_FILE_ATTRIBUTE_DATA data;
    wstr_init(&directory);
    wstr_init(&path);
    if (!nc_update_ensure_directory_w(&directory, &error) ||
        !nc_path_join_w(&path, wstr_cstr(&directory), L"update.log")) {
        wstr_free(&path);
        wstr_free(&directory);
        return;
    }
    memset(&data, 0, sizeof(data));
    if (GetFileAttributesExW(wstr_cstr(&path), GetFileExInfoStandard, &data)) {
        ULARGE_INTEGER size;
        size.LowPart = data.nFileSizeLow;
        size.HighPart = data.nFileSizeHigh;
        if (size.QuadPart > 512 * 1024) {
            WStr backup;
            wstr_init(&backup);
            if (wstr_set(&backup, wstr_cstr(&path)) && wstr_add(&backup, L".old")) {
                MoveFileExW(wstr_cstr(&path), wstr_cstr(&backup), MOVEFILE_REPLACE_EXISTING);
            }
            wstr_free(&backup);
        }
    }
    nc_format_local_iso_ms(stamp, sizeof(stamp));
    snprintf(line, sizeof(line), "[%s] %s\r\n", stamp, message ? message : "");
    nc_append_file(wstr_cstr(&path), line, strlen(line), &error);
    wstr_free(&path);
    wstr_free(&directory);
}

bool nc_update_is_dry_run(void) {
    char value[16];
    DWORD length = GetEnvironmentVariableA("NC_UPDATE_DRY_RUN", value, sizeof(value));
    return length == 1 && value[0] == '1';
}

bool nc_update_payload_path_w(const char *version, WStr *out) {
    char normalized[32];
    wchar_t wide_name[128];
    WStr directory;
    bool ok = false;
    if (!out) {
        return false;
    }
    wstr_clear(out);
    if (!nc_version_normalize(version, normalized, sizeof(normalized))) {
        snprintf(normalized, sizeof(normalized), "unknown");
    }
    swprintf(wide_name, sizeof(wide_name) / sizeof(wide_name[0]),
             L"NetConditioner-%hs-win-x64.exe", normalized);
    wstr_init(&directory);
    if (nc_update_directory_w(&directory)) {
        ok = nc_path_join_w(out, wstr_cstr(&directory), wide_name);
    }
    wstr_free(&directory);
    return ok;
}

void nc_update_payload_path(const char *version, wchar_t *out, size_t cap) {
    WStr path;
    if (!out || cap == 0) {
        return;
    }
    out[0] = 0;
    wstr_init(&path);
    if (nc_update_payload_path_w(version, &path)) {
        nc_copy_wide(out, cap, wstr_cstr(&path));
    }
    wstr_free(&path);
}

void nc_update_cleanup_old_files(void) {
    WStr directory;
    WStr pattern;
    WStr path;
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
    wstr_init(&directory);
    wstr_init(&pattern);
    wstr_init(&path);
    if (!nc_update_directory_w(&directory) ||
        !nc_path_join_w(&pattern, wstr_cstr(&directory), L"*")) {
        goto done;
    }
    find = FindFirstFileW(wstr_cstr(&pattern), &data);
    if (find == INVALID_HANDLE_VALUE) {
        goto done;
    }
    do {
        ULARGE_INTEGER written;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        written.LowPart = data.ftLastWriteTime.dwLowDateTime;
        written.HighPart = data.ftLastWriteTime.dwHighDateTime;
        if (written.QuadPart < cutoff.QuadPart) {
            if (nc_path_join_w(&path, wstr_cstr(&directory), data.cFileName)) {
                DeleteFileW(wstr_cstr(&path));
            }
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
done:
    wstr_free(&path);
    wstr_free(&pattern);
    wstr_free(&directory);
}
