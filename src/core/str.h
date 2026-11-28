#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

static_assert(__STDC_VERSION__ >= 202311L, "NetConditioner requires a C23 compiler");

/* UTF-8 <-> UTF-16 helpers. Returned pointers are malloc'd. */
wchar_t *nc_wide_dup(const char *utf8);
char *nc_utf8_dup(const wchar_t *wide);
bool nc_copy_wide(wchar_t *out, size_t cap, const wchar_t *text);
bool nc_write_utf8(wchar_t *buffer, size_t cap, const char *utf8);
bool nc_lower_wide(const wchar_t *input, wchar_t **output);

/* Dynamic UTF-8 string builder. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Str;

void str_init(Str *s);
void str_free(Str *s);
void str_clear(Str *s);
bool str_reserve(Str *s, size_t extra);
bool str_add(Str *s, const char *text);
bool str_addn(Str *s, const char *text, size_t len);
bool str_addf(Str *s, const char *fmt, ...);
const char *str_cstr(const Str *s);
char *str_take(Str *s);

/* Dynamic UTF-16 string/path builder (eliminates 64 KiB fixed path buffers). */
typedef struct {
    wchar_t *data;
    size_t len;
    size_t cap;
} WStr;

void wstr_init(WStr *s);
void wstr_free(WStr *s);
void wstr_clear(WStr *s);
bool wstr_reserve(WStr *s, size_t extra);
bool wstr_addn(WStr *s, const wchar_t *text, size_t len);
bool wstr_add(WStr *s, const wchar_t *text);
bool wstr_set(WStr *s, const wchar_t *text);
bool wstr_set_utf8(WStr *s, const char *utf8);
bool wstr_addf(WStr *s, const wchar_t *fmt, ...);
const wchar_t *wstr_cstr(const WStr *s);
bool wstr_empty(const WStr *s);
wchar_t *wstr_take(WStr *s);

/* Text and duplication helpers. */
char *nc_strdup(const char *text);
char *nc_strndup(const char *text, size_t len);
wchar_t *nc_wcsdup(const wchar_t *text);
char *nc_trim(char *text);
bool nc_ieq_ascii(const char *left, const char *right);
bool nc_istarts_ascii(const char *text, const char *prefix);

/* Dynamic UTF-8 string list. */
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} NcStringList;

void nc_string_list_init(NcStringList *list);
void nc_string_list_free(NcStringList *list);
void nc_string_list_clear(NcStringList *list);
bool nc_string_list_push(NcStringList *list, const char *text);
bool nc_string_list_push_owned(NcStringList *list, char *text);
bool nc_string_list_contains_ci(const NcStringList *list, const char *text);
void nc_string_list_remove_at(NcStringList *list, size_t index);

/* Numeric formatting and parsing. */
void nc_format_u64(uint64_t value, char *buf, size_t cap);
void nc_format_i32_grouped(int32_t value, char *buf, size_t cap);
bool nc_parse_i32_digits(const char *text, int32_t max, int32_t *out);
