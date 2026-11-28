#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/err.h"
#include "core/str.h"

/* Dynamic WStr-based path helpers (preferred; zero large stack buffers). */
bool nc_known_folder_w(REFKNOWNFOLDERID id, WStr *out);
bool nc_env_absolute_dir_w(const wchar_t *name, WStr *out);
bool nc_temp_dir_w(WStr *out);
bool nc_path_join_w(WStr *out, const wchar_t *a, const wchar_t *b);
bool nc_path_parent_w(const wchar_t *path, WStr *out);
bool nc_get_module_path_w(WStr *out);
bool nc_create_temp_file_w(const wchar_t *dir, const wchar_t *tag, const void *data, size_t size,
                           WStr *path_out, HANDLE *handle_out, NcErr *err);

/* Fixed-buffer path helpers. */
bool nc_known_folder(REFKNOWNFOLDERID id, wchar_t *out, size_t cap);
bool nc_env_absolute_dir(const wchar_t *name, wchar_t *out, size_t cap);
bool nc_path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b);
bool nc_path_parent(const wchar_t *path, wchar_t *out, size_t cap);
const wchar_t *nc_path_file_name(const wchar_t *path);
bool nc_get_module_path(wchar_t *out, size_t cap);

/* Directory and file operations. */
bool nc_file_exists(const wchar_t *path);
bool nc_dir_exists(const wchar_t *path);
bool nc_make_dirs(const wchar_t *path);
bool nc_delete_tree(const wchar_t *path);
bool nc_read_file(const wchar_t *path, size_t max_bytes, void **data, size_t *size, NcErr *err);
bool nc_write_file(const wchar_t *path, const void *data, size_t size, NcErr *err);
bool nc_append_file(const wchar_t *path, const void *data, size_t size, NcErr *err);
bool nc_replace_file(const wchar_t *source, const wchar_t *target, NcErr *err);
bool nc_create_temp_file(const wchar_t *dir, const wchar_t *tag, const void *data, size_t size,
                         wchar_t *path_out, size_t path_cap, HANDLE *handle_out, NcErr *err);
void nc_remove_file(const wchar_t *path);
