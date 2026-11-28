#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/time.h"

static bool wide_is_absolute(const wchar_t *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == L'\\' && path[1] == L'\\') {
        return true;
    }
    if (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':') {
        return true;
    }
    return false;
}

bool nc_known_folder_w(REFKNOWNFOLDERID id, WStr *out) {
    PWSTR path = NULL;
    HRESULT hr;
    bool ok;
    if (!out) {
        return false;
    }
    wstr_clear(out);
    hr = SHGetKnownFolderPath(id, 0, NULL, &path);
    if (FAILED(hr) || !path) {
        if (path) {
            CoTaskMemFree(path);
        }
        return false;
    }
    ok = wstr_set(out, path);
    CoTaskMemFree(path);
    return ok;
}

bool nc_known_folder(REFKNOWNFOLDERID id, wchar_t *out, size_t cap) {
    WStr path;
    bool ok;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    wstr_init(&path);
    ok = nc_known_folder_w(id, &path) && nc_copy_wide(out, cap, wstr_cstr(&path));
    wstr_free(&path);
    return ok;
}

bool nc_env_absolute_dir_w(const wchar_t *name, WStr *out) {
    DWORD needed;
    DWORD written;
    wchar_t *buffer;
    bool ok = false;
    if (!name || !out) {
        return false;
    }
    wstr_clear(out);
    needed = GetEnvironmentVariableW(name, NULL, 0);
    if (needed == 0) {
        return false;
    }
    buffer = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (!buffer) {
        return false;
    }
    written = GetEnvironmentVariableW(name, buffer, needed);
    if (written > 0 && written < needed && wide_is_absolute(buffer)) {
        ok = wstr_set(out, buffer);
    }
    free(buffer);
    return ok;
}

bool nc_env_absolute_dir(const wchar_t *name, wchar_t *out, size_t cap) {
    WStr value;
    bool ok;
    if (!name || !out || cap == 0) {
        return false;
    }
    out[0] = 0;
    wstr_init(&value);
    ok = nc_env_absolute_dir_w(name, &value) && nc_copy_wide(out, cap, wstr_cstr(&value));
    wstr_free(&value);
    return ok;
}

bool nc_temp_dir_w(WStr *out) {
    DWORD cap = 260;
    if (!out) {
        return false;
    }
    if (nc_env_absolute_dir_w(L"TEMP", out)) {
        return true;
    }
    wstr_clear(out);
    while (cap <= 32768) {
        wchar_t *buffer = (wchar_t *)malloc((size_t)cap * sizeof(wchar_t));
        DWORD len;
        if (!buffer) {
            return false;
        }
        len = GetTempPathW(cap, buffer);
        if (len > 0 && len < cap) {
            bool ok = wstr_set(out, buffer);
            free(buffer);
            return ok;
        }
        free(buffer);
        if (len == 0) {
            return false;
        }
        cap = len + 1;
    }
    return false;
}

bool nc_path_join_w(WStr *out, const wchar_t *a, const wchar_t *b) {
    WStr joined;
    bool ok = true;
    if (!out) {
        return false;
    }
    wstr_init(&joined);
    if (!a || !a[0]) {
        ok = wstr_set(&joined, b ? b : L"");
    } else if (!b || !b[0]) {
        ok = wstr_set(&joined, a);
    } else {
        size_t a_len = wcslen(a);
        ok = wstr_addn(&joined, a, a_len);
        if (ok && a[a_len - 1] != L'\\' && a[a_len - 1] != L'/') {
            ok = wstr_addn(&joined, L"\\", 1);
        }
        if (ok) {
            ok = wstr_add(&joined, b);
        }
    }
    if (ok) {
        wstr_free(out);
        *out = joined;
        return true;
    }
    wstr_free(&joined);
    return false;
}

bool nc_path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b) {
    size_t a_len;
    size_t b_len;
    size_t pos;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!a || !a[0]) {
        return nc_copy_wide(out, cap, b ? b : L"");
    }
    if (!b || !b[0]) {
        return nc_copy_wide(out, cap, a);
    }
    a_len = wcslen(a);
    b_len = wcslen(b);
    if (a_len >= cap) {
        return false;
    }
    memcpy(out, a, a_len * sizeof(wchar_t));
    pos = a_len;
    if (out[pos - 1] != L'\\' && out[pos - 1] != L'/') {
        if (pos + 1 >= cap) {
            return false;
        }
        out[pos++] = L'\\';
    }
    if (pos + b_len >= cap) {
        return false;
    }
    memcpy(out + pos, b, (b_len + 1) * sizeof(wchar_t));
    return true;
}

bool nc_path_parent_w(const wchar_t *path, WStr *out) {
    size_t len;
    size_t index;
    if (!path || !out) {
        return false;
    }
    len = wcslen(path);
    while (len > 0 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        len--;
    }
    if (len == 0) {
        wstr_clear(out);
        return false;
    }
    for (index = len; index > 0; index--) {
        if (path[index - 1] == L'\\' || path[index - 1] == L'/') {
            size_t parent_len = index - 1;
            WStr result;
            bool ok;
            while (parent_len > 0 && (path[parent_len - 1] == L'\\' || path[parent_len - 1] == L'/')) {
                parent_len--;
            }
            if (parent_len == 0) {
                if (path[0] == L'\\') {
                    return wstr_set(out, L"\\");
                }
                wstr_clear(out);
                return false;
            }
            wstr_init(&result);
            ok = wstr_addn(&result, path, parent_len);
            if (ok) {
                wstr_free(out);
                *out = result;
            } else {
                wstr_free(&result);
            }
            return ok;
        }
    }
    wstr_clear(out);
    return false;
}

bool nc_path_parent(const wchar_t *path, wchar_t *out, size_t cap) {
    WStr parent;
    bool ok;
    if (!path || !out || cap == 0) {
        return false;
    }
    out[0] = 0;
    wstr_init(&parent);
    ok = nc_path_parent_w(path, &parent) && nc_copy_wide(out, cap, wstr_cstr(&parent));
    wstr_free(&parent);
    return ok;
}

const wchar_t *nc_path_file_name(const wchar_t *path) {
    const wchar_t *file;
    const wchar_t *p;
    if (!path) {
        return L"";
    }
    file = path;
    for (p = path; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            file = p + 1;
        }
    }
    return file;
}

bool nc_get_module_path_w(WStr *out) {
    DWORD cap = 260;
    if (!out) {
        return false;
    }
    wstr_clear(out);
    while (cap <= 32768) {
        wchar_t *buffer = (wchar_t *)malloc((size_t)cap * sizeof(wchar_t));
        DWORD length;
        if (!buffer) {
            return false;
        }
        length = GetModuleFileNameW(NULL, buffer, cap);
        if (length == 0) {
            free(buffer);
            return false;
        }
        if (length < cap) {
            bool ok = wstr_addn(out, buffer, length);
            free(buffer);
            return ok;
        }
        free(buffer);
        if (cap == 32768) {
            break;
        }
        cap = cap * 2 <= 32768 ? cap * 2 : 32768;
    }
    return false;
}

bool nc_get_module_path(wchar_t *out, size_t cap) {
    DWORD length;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    length = GetModuleFileNameW(NULL, out, (DWORD)cap);
    return length > 0 && length < cap;
}

bool nc_file_exists(const wchar_t *path) {
    DWORD attrs;
    if (!path) {
        return false;
    }
    attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool nc_dir_exists(const wchar_t *path) {
    DWORD attrs;
    if (!path) {
        return false;
    }
    attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool nc_make_dirs(const wchar_t *path) {
    size_t len;
    size_t index;
    wchar_t *buffer;
    DWORD attrs;
    if (!path || !path[0]) {
        return false;
    }
    attrs = GetFileAttributesW(path);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    len = wcslen(path);
    buffer = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!buffer) {
        return false;
    }
    memcpy(buffer, path, (len + 1) * sizeof(wchar_t));
    for (index = 1; index < len; index++) {
        if (buffer[index] == L'\\' || buffer[index] == L'/') {
            wchar_t saved = buffer[index];
            buffer[index] = 0;
            CreateDirectoryW(buffer, NULL);
            buffer[index] = saved;
        }
    }
    if (!CreateDirectoryW(buffer, NULL)) {
        attrs = GetFileAttributesW(path);
        free(buffer);
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    free(buffer);
    return true;
}

bool nc_delete_tree(const wchar_t *path) {
    WStr pattern;
    WStr child;
    WIN32_FIND_DATAW data;
    HANDLE find;
    bool ok = true;
    if (!path || !path[0]) {
        return false;
    }
    wstr_init(&pattern);
    wstr_init(&child);
    if (!nc_path_join_w(&pattern, path, L"*")) {
        wstr_free(&pattern);
        wstr_free(&child);
        return false;
    }
    find = FindFirstFileW(wstr_cstr(&pattern), &data);
    wstr_free(&pattern);
    if (find == INVALID_HANDLE_VALUE) {
        wstr_free(&child);
        return RemoveDirectoryW(path) != 0;
    }
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        if (!nc_path_join_w(&child, path, data.cFileName)) {
            ok = false;
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!nc_delete_tree(wstr_cstr(&child))) {
                ok = false;
            }
        } else if (!DeleteFileW(wstr_cstr(&child))) {
            ok = false;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    wstr_free(&child);
    if (!RemoveDirectoryW(path)) {
        ok = false;
    }
    return ok;
}

static bool read_all(HANDLE file, void *buffer, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = (DWORD)((size - offset) > 0x40000000u ? 0x40000000u : (size - offset));
        DWORD read = 0;
        if (!ReadFile(file, (unsigned char *)buffer + offset, chunk, &read, NULL) || read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

static bool write_all(HANDLE file, const void *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = (DWORD)((size - offset) > 0x40000000u ? 0x40000000u : (size - offset));
        DWORD written = 0;
        if (!WriteFile(file, (const unsigned char *)data + offset, chunk, &written, NULL) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool nc_read_file(const wchar_t *path, size_t max_bytes, void **data, size_t *size, NcErr *err) {
    HANDLE file;
    LARGE_INTEGER length;
    void *buffer;
    if (!path || !data || !size) {
        return nc_fail(err, "读取文件参数无效。");
    }
    *data = NULL;
    *size = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return nc_fail(err, "打开文件失败（Win32 错误 %lu）：%ls", (unsigned long)GetLastError(), path);
    }
    if (!GetFileSizeEx(file, &length)) {
        DWORD code = GetLastError();
        CloseHandle(file);
        return nc_fail(err, "读取文件大小失败（Win32 错误 %lu）。", (unsigned long)code);
    }
    if (length.QuadPart < 0 || (uint64_t)length.QuadPart > (uint64_t)max_bytes) {
        CloseHandle(file);
        return nc_fail(err, "文件超过大小限制。");
    }
    buffer = malloc(length.QuadPart > 0 ? (size_t)length.QuadPart : 1);
    if (!buffer) {
        CloseHandle(file);
        return nc_fail(err, "内存不足。");
    }
    if (length.QuadPart > 0 && !read_all(file, buffer, (size_t)length.QuadPart)) {
        DWORD code = GetLastError();
        free(buffer);
        CloseHandle(file);
        return nc_fail(err, "读取文件失败（Win32 错误 %lu）。", (unsigned long)code);
    }
    CloseHandle(file);
    *data = buffer;
    *size = (size_t)length.QuadPart;
    return true;
}

bool nc_write_file(const wchar_t *path, const void *data, size_t size, NcErr *err) {
    HANDLE file;
    if (!path) {
        return nc_fail(err, "写入路径无效。");
    }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return nc_fail(err, "创建文件失败（Win32 错误 %lu）：%ls", (unsigned long)GetLastError(), path);
    }
    if (size > 0 && !write_all(file, data, size)) {
        DWORD code = GetLastError();
        CloseHandle(file);
        return nc_fail(err, "写入文件失败（Win32 错误 %lu）。", (unsigned long)code);
    }
    if (!FlushFileBuffers(file)) {
        DWORD code = GetLastError();
        CloseHandle(file);
        return nc_fail(err, "同步文件失败（Win32 错误 %lu）。", (unsigned long)code);
    }
    CloseHandle(file);
    return true;
}

bool nc_append_file(const wchar_t *path, const void *data, size_t size, NcErr *err) {
    HANDLE file;
    if (!path) {
        return nc_fail(err, "写入路径无效。");
    }
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return nc_fail(err, "打开文件失败（Win32 错误 %lu）：%ls", (unsigned long)GetLastError(), path);
    }
    if (size > 0 && !write_all(file, data, size)) {
        DWORD code = GetLastError();
        CloseHandle(file);
        return nc_fail(err, "追加文件失败（Win32 错误 %lu）。", (unsigned long)code);
    }
    CloseHandle(file);
    return true;
}

bool nc_replace_file(const wchar_t *source, const wchar_t *target, NcErr *err) {
    if (!source || !target) {
        return nc_fail(err, "替换文件参数无效。");
    }
    if (!MoveFileExW(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return nc_fail(err, "替换文件失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
    }
    return true;
}

void nc_remove_file(const wchar_t *path) {
    if (path) {
        DeleteFileW(path);
    }
}

bool nc_create_temp_file_w(const wchar_t *dir, const wchar_t *tag, const void *data, size_t size,
                           WStr *path_out, HANDLE *handle_out, NcErr *err) {
    wchar_t base[512];
    WStr path;
    int attempt;
    if (!dir || !tag || !path_out) {
        return nc_fail(err, "临时文件参数无效。");
    }
    wstr_clear(path_out);
    wstr_init(&path);
    for (attempt = 0; attempt < 32; attempt++) {
        char id[33];
        wchar_t wide_id[64];
        HANDLE file;
        nc_new_id(id);
        nc_write_utf8(wide_id, sizeof(wide_id) / sizeof(wide_id[0]), id);
        swprintf(base, sizeof(base) / sizeof(base[0]), L"%ls-%ls.tmp", tag, wide_id);
        if (!nc_path_join_w(&path, dir, base)) {
            wstr_free(&path);
            return nc_fail(err, "临时文件路径构造失败。");
        }
        file = CreateFileW(wstr_cstr(&path), GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
                continue;
            }
            wstr_free(&path);
            return nc_fail(err, "创建临时文件失败（Win32 错误 %lu）。", (unsigned long)code);
        }
        if (size > 0 && !write_all(file, data, size)) {
            DWORD code = GetLastError();
            CloseHandle(file);
            nc_remove_file(wstr_cstr(&path));
            wstr_free(&path);
            return nc_fail(err, "写入临时文件失败（Win32 错误 %lu）。", (unsigned long)code);
        }
        if (!FlushFileBuffers(file)) {
            DWORD code = GetLastError();
            CloseHandle(file);
            nc_remove_file(wstr_cstr(&path));
            wstr_free(&path);
            return nc_fail(err, "同步临时文件失败（Win32 错误 %lu）。", (unsigned long)code);
        }
        if (handle_out) {
            *handle_out = file;
        } else {
            CloseHandle(file);
        }
        wstr_free(path_out);
        *path_out = path;
        return true;
    }
    wstr_free(&path);
    return nc_fail(err, "无法创建唯一临时文件。");
}

bool nc_create_temp_file(const wchar_t *dir, const wchar_t *tag, const void *data, size_t size,
                         wchar_t *path_out, size_t path_cap, HANDLE *handle_out, NcErr *err) {
    WStr path;
    HANDLE file = INVALID_HANDLE_VALUE;
    if (!path_out || path_cap == 0) {
        return nc_fail(err, "临时文件参数无效。");
    }
    path_out[0] = 0;
    wstr_init(&path);
    if (!nc_create_temp_file_w(dir, tag, data, size, &path, handle_out ? &file : NULL, err)) {
        wstr_free(&path);
        return false;
    }
    if (!nc_copy_wide(path_out, path_cap, wstr_cstr(&path))) {
        if (handle_out && file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        nc_remove_file(wstr_cstr(&path));
        wstr_free(&path);
        return nc_fail(err, "临时文件路径过长。");
    }
    if (handle_out) {
        *handle_out = file;
    }
    wstr_free(&path);
    return true;
}
