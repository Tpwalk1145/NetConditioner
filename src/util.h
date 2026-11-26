#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define NC_ERR_CAP 1024

typedef struct {
    char msg[NC_ERR_CAP];
} NcErr;

void nc_err_vset(NcErr *err, const char *fmt, va_list ap);
void nc_err_set(NcErr *err, const char *fmt, ...);
bool nc_fail(NcErr *err, const char *fmt, ...);

/* UTF-8 <-> UTF-16 helpers. Returned pointers are malloc'd. */
wchar_t *nc_wide_dup(const char *utf8);
char *nc_utf8_dup(const wchar_t *wide);
bool nc_copy_wide(wchar_t *out, size_t cap, const wchar_t *text);
bool nc_write_utf8(wchar_t *buffer, size_t cap, const char *utf8);

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
const char *str_cstr(Str *s);
char *str_take(Str *s);

/* Text helpers. */
char *nc_trim(char *text);
bool nc_ieq_ascii(const char *left, const char *right);
bool nc_istarts_ascii(const char *text, const char *prefix);

/* Process name normalization: lowercase, no path, no .exe. */
void nc_normalize_process_name(const char *raw, Str *out);
void nc_normalize_display_name(const char *raw, Str *out);

/* Paths. */
bool nc_known_folder(REFKNOWNFOLDERID id, wchar_t *out, size_t cap);
bool nc_env_absolute_dir(const wchar_t *name, wchar_t *out, size_t cap);
bool nc_path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b);
bool nc_path_parent(const wchar_t *path, wchar_t *out, size_t cap);
const wchar_t *nc_path_file_name(const wchar_t *path);
bool nc_file_exists(const wchar_t *path);
bool nc_dir_exists(const wchar_t *path);
bool nc_make_dirs(const wchar_t *path);
bool nc_get_module_path(wchar_t *out, size_t cap);

/* Files. */
bool nc_read_file(const wchar_t *path, size_t max_bytes, void **data, size_t *size, NcErr *err);
bool nc_write_file(const wchar_t *path, const void *data, size_t size, NcErr *err);
bool nc_append_file(const wchar_t *path, const void *data, size_t size, NcErr *err);
bool nc_replace_file(const wchar_t *source, const wchar_t *target, NcErr *err);
bool nc_create_temp_file(const wchar_t *dir, const wchar_t *tag, const void *data, size_t size,
                         wchar_t *path_out, size_t path_cap, HANDLE *handle_out, NcErr *err);
void nc_remove_file(const wchar_t *path);

/* Strings. */
char *nc_strdup(const char *text);
char *nc_strndup(const char *text, size_t len);

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

/* Hashing. */
uint64_t nc_fnv1a_bytes(const void *data, size_t size);
uint64_t nc_fnv1a_utf8(const char *text);
uint64_t nc_fnv1a_wide(const wchar_t *text);

/* Time and randomness. */
int64_t nc_now_unix(void);
void nc_format_local_iso_ms(char *buf, size_t cap);
void nc_format_local_hms_ms(char *buf, size_t cap);
void nc_format_local_hms(char *buf, size_t cap);
uint64_t nc_monotonic_ns(void);
uint64_t nc_random_seed(void);
uint64_t nc_rand_next(uint64_t *state);
uint32_t nc_rand_below(uint64_t *state, uint32_t bound);
uint64_t nc_rand_inclusive(uint64_t *state, uint64_t max);
void nc_new_id(char out[33]);

/* Numbers. */
void nc_format_u64(uint64_t value, char *buf, size_t cap);
void nc_format_i32_grouped(int32_t value, char *buf, size_t cap);
bool nc_parse_i32_digits(const char *text, int32_t max, int32_t *out);