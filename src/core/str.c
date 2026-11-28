#include "core/str.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool nc_lower_wide(const wchar_t *input, wchar_t **output) {
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

const char *str_cstr(const Str *s) {
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

void wstr_init(WStr *s) {
    if (!s) {
        return;
    }
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void wstr_free(WStr *s) {
    if (!s) {
        return;
    }
    free(s->data);
    wstr_init(s);
}

void wstr_clear(WStr *s) {
    if (!s) {
        return;
    }
    s->len = 0;
    if (s->data) {
        s->data[0] = 0;
    }
}

bool wstr_reserve(WStr *s, size_t extra) {
    size_t need;
    size_t cap;
    wchar_t *data;
    if (!s || extra > (SIZE_MAX / sizeof(wchar_t)) - s->len - 1) {
        return false;
    }
    need = s->len + extra + 1;
    if (need <= s->cap) {
        return true;
    }
    cap = s->cap ? s->cap : 64;
    while (cap < need) {
        if (cap > (SIZE_MAX / sizeof(wchar_t)) / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    data = (wchar_t *)realloc(s->data, cap * sizeof(wchar_t));
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

bool wstr_addn(WStr *s, const wchar_t *text, size_t len) {
    if (!s || (!text && len > 0)) {
        return false;
    }
    if (!wstr_reserve(s, len)) {
        return false;
    }
    if (len > 0) {
        memcpy(s->data + s->len, text, len * sizeof(wchar_t));
    }
    s->len += len;
    s->data[s->len] = 0;
    return true;
}

bool wstr_add(WStr *s, const wchar_t *text) {
    return text ? wstr_addn(s, text, wcslen(text)) : false;
}

bool wstr_set(WStr *s, const wchar_t *text) {
    if (!s) {
        return false;
    }
    wstr_clear(s);
    if (!text) {
        return true;
    }
    return wstr_add(s, text);
}

bool wstr_set_utf8(WStr *s, const char *utf8) {
    wchar_t *wide;
    bool ok;
    if (!s) {
        return false;
    }
    wstr_clear(s);
    if (!utf8 || !utf8[0]) {
        return true;
    }
    wide = nc_wide_dup(utf8);
    if (!wide) {
        return false;
    }
    ok = wstr_add(s, wide);
    free(wide);
    return ok;
}

bool wstr_addf(WStr *s, const wchar_t *fmt, ...) {
    wchar_t stack_buffer[256];
    wchar_t *big;
    va_list ap;
    int needed;
    bool ok;
    if (!s || !fmt) {
        return false;
    }
    va_start(ap, fmt);
    needed = _vscwprintf(fmt, ap);
    va_end(ap);
    if (needed < 0) {
        return false;
    }
    if ((size_t)needed < sizeof(stack_buffer) / sizeof(stack_buffer[0])) {
        va_start(ap, fmt);
        _vsnwprintf(stack_buffer, sizeof(stack_buffer) / sizeof(stack_buffer[0]), fmt, ap);
        va_end(ap);
        return wstr_addn(s, stack_buffer, (size_t)needed);
    }
    big = (wchar_t *)malloc(((size_t)needed + 1) * sizeof(wchar_t));
    if (!big) {
        return false;
    }
    va_start(ap, fmt);
    _vsnwprintf(big, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    ok = wstr_addn(s, big, (size_t)needed);
    free(big);
    return ok;
}

const wchar_t *wstr_cstr(const WStr *s) {
    return (s && s->data) ? s->data : L"";
}

bool wstr_empty(const WStr *s) {
    return !s || s->len == 0;
}

wchar_t *wstr_take(WStr *s) {
    wchar_t *data;
    if (!s) {
        return NULL;
    }
    if (!s->data) {
        s->data = (wchar_t *)malloc(sizeof(wchar_t));
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

wchar_t *nc_wcsdup(const wchar_t *text) {
    size_t len;
    wchar_t *copy;
    if (!text) {
        return NULL;
    }
    len = wcslen(text);
    copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, (len + 1) * sizeof(wchar_t));
    return copy;
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
