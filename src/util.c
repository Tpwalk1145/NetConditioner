#include "util.h"

#include <winnls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void nc_err_vset(NcErr *err, const char *fmt, va_list ap) {
    if (!err) {
        return;
    }
    if (!fmt) {
        err->msg[0] = 0;
        return;
    }
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    err->msg[sizeof(err->msg) - 1] = 0;
}

void nc_err_set(NcErr *err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nc_err_vset(err, fmt, ap);
    va_end(ap);
}

bool nc_fail(NcErr *err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nc_err_vset(err, fmt, ap);
    va_end(ap);
    return false;
}

wchar_t *nc_wide_dup(const char *utf8) {
    int need;
    wchar_t *out;
    if (!utf8) {
        return NULL;
    }
    need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    out = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!out) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, need) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

char *nc_utf8_dup(const wchar_t *wide) {
    int need;
    char *out;
    if (!wide) {
        return NULL;
    }
    need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (need <= 0) {
        return NULL;
    }
    out = (char *)malloc((size_t)need);
    if (!out) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, need, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

bool nc_copy_wide(wchar_t *out, size_t cap, const wchar_t *text) {
    size_t len;
    if (!out || cap == 0) {
        return false;
    }
    if (!text) {
        out[0] = 0;
        return true;
    }
    len = wcslen(text);
    if (len >= cap) {
        memcpy(out, text, (cap - 1) * sizeof(wchar_t));
        out[cap - 1] = 0;
        return false;
    }
    memcpy(out, text, (len + 1) * sizeof(wchar_t));
    return true;
}

bool nc_write_utf8(wchar_t *buffer, size_t cap, const char *utf8) {
    wchar_t *wide;
    size_t units;
    size_t copy;
    if (!buffer || cap == 0) {
        return false;
    }
    memset(buffer, 0, cap * sizeof(wchar_t));
    wide = nc_wide_dup(utf8 ? utf8 : "");
    if (!wide) {
        return false;
    }
    units = wcslen(wide);
    copy = units < cap - 1 ? units : cap - 1;
    memcpy(buffer, wide, copy * sizeof(wchar_t));
    buffer[copy] = 0;
    free(wide);
    return units <= cap - 1;
}

void str_init(Str *s) {
    if (!s) {
        return;
    }
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void str_free(Str *s) {
    if (!s) {
        return;
    }
    free(s->data);
    str_init(s);
}

void str_clear(Str *s) {
    if (!s) {
        return;
    }
    s->len = 0;
    if (s->data) {
        s->data[0] = 0;
    }
}

bool str_reserve(Str *s, size_t extra) {
    size_t need;
    size_t cap;
    char *data;
    if (!s || extra > SIZE_MAX - s->len - 1) {
        return false;
    }
    need = s->len + extra + 1;
    if (need <= s->cap) {
        return true;
    }
    cap = s->cap ? s->cap : 32;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    data = (char *)realloc(s->data, cap);
    if (!data) {
        return false;
    }
    s->data = data;
    s->cap = cap;
    if (s->len == 0) {
        s->data[0] = 0;
    }
    return true;
}

bool str_addn(Str *s, const char *text, size_t len) {
    if (!s || (!text && len > 0)) {
        return false;
    }
    if (!str_reserve(s, len)) {
        return false;
    }
    if (len > 0) {
        memcpy(s->data + s->len, text, len);
    }
    s->len += len;
    s->data[s->len] = 0;
    return true;
}

bool str_add(Str *s, const char *text) {
    return text ? str_addn(s, text, strlen(text)) : false;
}

bool str_addf(Str *s, const char *fmt, ...) {
    char stack_buffer[512];
    char *big;
    va_list ap;
    int needed;
    bool ok;
    if (!s || !fmt) {
        return false;
    }
    va_start(ap, fmt);
    needed = vsnprintf(stack_buffer, sizeof(stack_buffer), fmt, ap);
    va_end(ap);
    if (needed < 0) {
        return false;
    }
    if ((size_t)needed < sizeof(stack_buffer)) {
        return str_addn(s, stack_buffer, (size_t)needed);
    }
    big = (char *)malloc((size_t)needed + 1);
    if (!big) {
        return false;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    ok = str_addn(s, big, (size_t)needed);
    free(big);
    return ok;
}

const char *str_cstr(Str *s) {
    return (s && s->data) ? s->data : "";
}

char *str_take(Str *s) {
    char *data;
    if (!s) {
        return NULL;
    }
    if (!s->data) {
        s->data = (char *)malloc(1);
        if (!s->data) {
            return NULL;
        }
        s->data[0] = 0;
        s->cap = 1;
    }
    data = s->data;
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
    return data;
}

char *nc_trim(char *text) {
    char *start;
    size_t len;
    if (!text) {
        return text;
    }
    start = text;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = 0;
    }
    return text;
}

bool nc_ieq_ascii(const char *left, const char *right) {
    if (!left || !right) {
        return left == right;
    }
    return _stricmp(left, right) == 0;
}

bool nc_istarts_ascii(const char *text, const char *prefix) {
    if (!text || !prefix) {
        return false;
    }
    return _strnicmp(text, prefix, strlen(prefix)) == 0;
}

static bool lower_wide(const wchar_t *input, wchar_t **output) {
    int needed;
    wchar_t *buffer;
    if (!input || !output) {
        return false;
    }
    needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, input, -1, NULL, 0, NULL, NULL, 0);
    if (needed <= 0) {
        return false;
    }
    buffer = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (!buffer) {
        return false;
    }
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, input, -1, buffer, needed, NULL, NULL, 0) <= 0) {
        free(buffer);
        return false;
    }
    *output = buffer;
    return true;
}

void nc_normalize_process_name(const char *raw, Str *out) {
    const char *start;
    const char *end;
    const char *file;
    const char *p;
    size_t len;
    char *piece;
    wchar_t *wide;
    wchar_t *lower;
    char *utf8;
    if (!out) {
        return;
    }
    str_clear(out);
    if (!raw) {
        return;
    }
    start = raw;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    file = start;
    for (p = start; p < end; p++) {
        if (*p == '\\' || *p == '/') {
            file = p + 1;
        }
    }
    len = (size_t)(end - file);
    if (len >= 4 && _strnicmp(file + len - 4, ".exe", 4) == 0) {
        len -= 4;
    }
    while (len > 0 && isspace((unsigned char)file[0])) {
        file++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)file[len - 1])) {
        len--;
    }
    if (len == 0) {
        return;
    }
    piece = (char *)malloc(len + 1);
    if (!piece) {
        return;
    }
    memcpy(piece, file, len);
    piece[len] = 0;
    wide = nc_wide_dup(piece);
    free(piece);
    if (!wide) {
        return;
    }
    lower = NULL;
    if (!lower_wide(wide, &lower)) {
        lower = wide;
        wide = NULL;
    }
    free(wide);
    utf8 = nc_utf8_dup(lower);
    free(lower);
    if (utf8) {
        str_add(out, utf8);
        free(utf8);
    }
}

void nc_normalize_display_name(const char *raw, Str *out) {
    Str name;
    if (!out) {
        return;
    }
    str_clear(out);
    str_init(&name);
    nc_normalize_process_name(raw, &name);
    if (name.len > 0) {
        str_add(out, str_cstr(&name));
        str_add(out, ".exe");
    }
    str_free(&name);
}

bool nc_known_folder(REFKNOWNFOLDERID id, wchar_t *out, size_t cap) {
    PWSTR path = NULL;
    HRESULT hr;
    bool ok;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    hr = SHGetKnownFolderPath(id, 0, NULL, &path);
    if (FAILED(hr) || !path) {
        if (path) {
            CoTaskMemFree(path);
        }
        return false;
    }
    ok = nc_copy_wide(out, cap, path);
    CoTaskMemFree(path);
    return ok;
}

static bool wide_is_absolute(const wchar_t *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '\\' && path[1] == '\\') {
        return true;
    }
    if (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':') {
        return true;
    }
    return false;
}

bool nc_env_absolute_dir(const wchar_t *name, wchar_t *out, size_t cap) {
    wchar_t buffer[32768];
    DWORD length;
    if (!name || !out || cap == 0) {
        return false;
    }
    out[0] = 0;
    length = GetEnvironmentVariableW(name, buffer, (DWORD)(sizeof(buffer) / sizeof(buffer[0])));
    if (length == 0 || length >= sizeof(buffer) / sizeof(buffer[0])) {
        return false;
    }
    if (!wide_is_absolute(buffer)) {
        return false;
    }
    return nc_copy_wide(out, cap, buffer);
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
    pos = 0;
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

bool nc_path_parent(const wchar_t *path, wchar_t *out, size_t cap) {
    size_t len;
    size_t index;
    if (!path || !out || cap == 0) {
        return false;
    }
    out[0] = 0;
    len = wcslen(path);
    while (len > 0 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        len--;
    }
    if (len == 0) {
        return false;
    }
    for (index = len; index > 0; index--) {
        if (path[index - 1] == L'\\' || path[index - 1] == L'/') {
            size_t parent_len = index - 1;
            while (parent_len > 0 && (path[parent_len - 1] == L'\\' || path[parent_len - 1] == L'/')) {
                parent_len--;
            }
            if (parent_len == 0) {
                if (path[0] == L'\\') {
                    return nc_copy_wide(out, cap, L"\\");
                }
                return false;
            }
            if (parent_len >= cap) {
                return false;
            }
            memcpy(out, path, parent_len * sizeof(wchar_t));
            out[parent_len] = 0;
            return true;
        }
    }
    return false;
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

bool nc_get_module_path(wchar_t *out, size_t cap) {
    DWORD length;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    length = GetModuleFileNameW(NULL, out, (DWORD)cap);
    return length > 0 && length < cap;
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

bool nc_create_temp_file(const wchar_t *dir, const wchar_t *tag, const void *data, size_t size,
                         wchar_t *path_out, size_t path_cap, HANDLE *handle_out, NcErr *err) {
    wchar_t base[512];
    int attempt;
    if (!dir || !tag || !path_out) {
        return nc_fail(err, "临时文件参数无效。");
    }
    path_out[0] = 0;
    for (attempt = 0; attempt < 32; attempt++) {
        char id[33];
        wchar_t wide_id[64];
        wchar_t path[32768];
        HANDLE file;
        nc_new_id(id);
        nc_write_utf8(wide_id, sizeof(wide_id) / sizeof(wide_id[0]), id);
        swprintf(base, sizeof(base) / sizeof(base[0]), L"%ls-%ls.tmp", tag, wide_id);
        if (!nc_path_join(path, sizeof(path) / sizeof(path[0]), dir, base)) {
            return nc_fail(err, "临时文件路径过长。");
        }
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
                continue;
            }
            return nc_fail(err, "创建临时文件失败（Win32 错误 %lu）。", (unsigned long)code);
        }
        if (size > 0 && !write_all(file, data, size)) {
            DWORD code = GetLastError();
            CloseHandle(file);
            nc_remove_file(path);
            return nc_fail(err, "写入临时文件失败（Win32 错误 %lu）。", (unsigned long)code);
        }
        if (!FlushFileBuffers(file)) {
            DWORD code = GetLastError();
            CloseHandle(file);
            nc_remove_file(path);
            return nc_fail(err, "同步临时文件失败（Win32 错误 %lu）。", (unsigned long)code);
        }
        if (handle_out) {
            *handle_out = file;
        } else {
            CloseHandle(file);
        }
        if (!nc_copy_wide(path_out, path_cap, path)) {
            if (handle_out) {
                CloseHandle(*handle_out);
                *handle_out = INVALID_HANDLE_VALUE;
            }
            nc_remove_file(path);
            return nc_fail(err, "临时文件路径过长。");
        }
        return true;
    }
    return nc_fail(err, "无法创建唯一临时文件。");
}

char *nc_strdup(const char *text) {
    size_t len;
    char *copy;
    if (!text) {
        return NULL;
    }
    len = strlen(text);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, len + 1);
    return copy;
}

char *nc_strndup(const char *text, size_t len) {
    char *copy;
    if (!text) {
        return NULL;
    }
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, len);
    copy[len] = 0;
    return copy;
}

void nc_string_list_init(NcStringList *list) {
    if (list) {
        list->items = NULL;
        list->count = 0;
        list->cap = 0;
    }
}

void nc_string_list_free(NcStringList *list) {
    size_t index;
    if (!list) {
        return;
    }
    for (index = 0; index < list->count; index++) {
        free(list->items[index]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

void nc_string_list_clear(NcStringList *list) {
    size_t index;
    if (!list) {
        return;
    }
    for (index = 0; index < list->count; index++) {
        free(list->items[index]);
    }
    list->count = 0;
}

static bool string_list_reserve(NcStringList *list, size_t extra) {
    size_t need;
    size_t cap;
    char **items;
    if (!list || extra > SIZE_MAX - list->count) {
        return false;
    }
    need = list->count + extra;
    if (need <= list->cap) {
        return true;
    }
    cap = list->cap ? list->cap : 8;
    while (cap < need) {
        cap *= 2;
    }
    items = (char **)realloc(list->items, cap * sizeof(char *));
    if (!items) {
        return false;
    }
    list->items = items;
    list->cap = cap;
    return true;
}

bool nc_string_list_push(NcStringList *list, const char *text) {
    char *copy;
    if (!list || !text) {
        return false;
    }
    if (!string_list_reserve(list, 1)) {
        return false;
    }
    copy = nc_strdup(text);
    if (!copy) {
        return false;
    }
    list->items[list->count++] = copy;
    return true;
}

bool nc_string_list_push_owned(NcStringList *list, char *text) {
    if (!list || !text) {
        return false;
    }
    if (!string_list_reserve(list, 1)) {
        return false;
    }
    list->items[list->count++] = text;
    return true;
}

bool nc_string_list_contains_ci(const NcStringList *list, const char *text) {
    size_t index;
    if (!list || !text) {
        return false;
    }
    for (index = 0; index < list->count; index++) {
        if (nc_ieq_ascii(list->items[index], text)) {
            return true;
        }
    }
    return false;
}

void nc_string_list_remove_at(NcStringList *list, size_t index) {
    if (!list || index >= list->count) {
        return;
    }
    free(list->items[index]);
    memmove(list->items + index, list->items + index + 1,
            (list->count - index - 1) * sizeof(char *));
    list->count--;
}

uint64_t nc_fnv1a_bytes(const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    uint64_t hash = 14695981039346656037ull;
    size_t index;
    for (index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t nc_fnv1a_utf8(const char *text) {
    return text ? nc_fnv1a_bytes(text, strlen(text)) : 14695981039346656037ull;
}

uint64_t nc_fnv1a_wide(const wchar_t *text) {
    return text ? nc_fnv1a_bytes(text, wcslen(text) * sizeof(wchar_t)) : 14695981039346656037ull;
}

int64_t nc_now_unix(void) {
    FILETIME file_time;
    ULARGE_INTEGER value;
    const uint64_t epoch = 116444736000000000ull;
    GetSystemTimeAsFileTime(&file_time);
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (value.QuadPart < epoch) {
        return 0;
    }
    return (int64_t)((value.QuadPart - epoch) / 10000000ull);
}

void nc_format_local_iso_ms(char *buf, size_t cap) {
    SYSTEMTIME st;
    if (!buf || cap == 0) {
        return;
    }
    GetLocalTime(&st);
    snprintf(buf, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
             (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
}

void nc_format_local_hms_ms(char *buf, size_t cap) {
    SYSTEMTIME st;
    if (!buf || cap == 0) {
        return;
    }
    GetLocalTime(&st);
    snprintf(buf, cap, "%02u:%02u:%02u.%03u",
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
}

void nc_format_local_hms(char *buf, size_t cap) {
    SYSTEMTIME st;
    if (!buf || cap == 0) {
        return;
    }
    GetLocalTime(&st);
    snprintf(buf, cap, "%02u:%02u:%02u",
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
}

static BOOL CALLBACK monotonic_init(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    LARGE_INTEGER *state = (LARGE_INTEGER *)parameter;
    (void)once;
    (void)context;
    QueryPerformanceFrequency(&state[0]);
    QueryPerformanceCounter(&state[1]);
    return TRUE;
}

uint64_t nc_monotonic_ns(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static LARGE_INTEGER state[2];
    LARGE_INTEGER now;
    uint64_t delta;
    uint64_t freq;
    InitOnceExecuteOnce(&once, monotonic_init, state, NULL);
    if (state[0].QuadPart <= 0) {
        return 0;
    }
    QueryPerformanceCounter(&now);
    if (now.QuadPart < state[1].QuadPart) {
        return 0;
    }
    delta = (uint64_t)(now.QuadPart - state[1].QuadPart);
    freq = (uint64_t)state[0].QuadPart;
    return (delta / freq) * 1000000000ull + (delta % freq) * 1000000000ull / freq;
}

uint64_t nc_random_seed(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)counter.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^
           ((uint64_t)GetCurrentThreadId() << 16) ^ GetTickCount64();
}

uint64_t nc_rand_next(uint64_t *state) {
    uint64_t value;
    if (!state || *state == 0) {
        uint64_t seed = nc_random_seed();
        if (state) {
            *state = seed ? seed : 0x9E3779B97F4A7C15ull;
        }
        if (!state) {
            return seed;
        }
    }
    value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * 2685821657736338717ull;
}

uint32_t nc_rand_below(uint64_t *state, uint32_t bound) {
    if (bound == 0) {
        return 0;
    }
    return (uint32_t)(nc_rand_next(state) % bound);
}

uint64_t nc_rand_inclusive(uint64_t *state, uint64_t max) {
    if (max == UINT64_MAX) {
        return nc_rand_next(state);
    }
    return nc_rand_next(state) % (max + 1);
}

void nc_new_id(char out[33]) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint64_t state = 0;
    int index;
    if (!out) {
        return;
    }
    AcquireSRWLockExclusive(&lock);
    if (state == 0) {
        state = nc_random_seed();
        if (state == 0) {
            state = 0x9E3779B97F4A7C15ull;
        }
    }
    for (index = 0; index < 4; index++) {
        uint32_t value = (uint32_t)(nc_rand_next(&state) >> 16);
        snprintf(out + index * 8, 9, "%08x", value);
    }
    ReleaseSRWLockExclusive(&lock);
}

void nc_format_u64(uint64_t value, char *buf, size_t cap) {
    if (buf && cap) {
        snprintf(buf, cap, "%llu", (unsigned long long)value);
    }
}

static void group_decimal(const char *digits, char *out, size_t cap) {
    size_t length = strlen(digits);
    size_t index;
    size_t pos = 0;
    if (!out || cap == 0) {
        return;
    }
    for (index = 0; index < length; index++) {
        if (index > 0 && (length - index) % 3 == 0 && pos + 1 < cap) {
            out[pos++] = ',';
        }
        if (pos + 1 < cap) {
            out[pos++] = digits[index];
        }
    }
    out[pos] = 0;
}

void nc_format_i32_grouped(int32_t value, char *buf, size_t cap) {
    char digits[16];
    char grouped[32];
    long long magnitude = value;
    if (!buf || cap == 0) {
        return;
    }
    if (magnitude < 0) {
        magnitude = -magnitude;
    }
    snprintf(digits, sizeof(digits), "%lld", magnitude);
    group_decimal(digits, grouped, sizeof(grouped));
    if (value < 0) {
        snprintf(buf, cap, "-%s", grouped);
    } else {
        snprintf(buf, cap, "%s", grouped);
    }
}

bool nc_parse_i32_digits(const char *text, int32_t max, int32_t *out) {
    const char *p = text ? text : "";
    uint64_t value = 0;
    bool overflow = false;
    uint64_t limit = max > 0 ? (uint64_t)max : 0;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
            p++;
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        if (!overflow) {
            value = value * 10 + (uint64_t)(c - '0');
            if (value > limit) {
                overflow = true;
            }
        }
        p++;
    }
    if (out) {
        *out = overflow ? max : (int32_t)value;
    }
    return true;
}