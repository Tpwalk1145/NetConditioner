#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core/err.h"
#include "core/str.h"

bool nc_update_directory_w(WStr *out);
bool nc_update_directory(wchar_t *out, size_t cap);
bool nc_update_ensure_directory_w(WStr *out, NcErr *err);
bool nc_update_ensure_directory(wchar_t *out, size_t cap, NcErr *err);
void nc_update_log(const char *message);
bool nc_update_is_dry_run(void);
bool nc_update_payload_path_w(const char *version, WStr *out);
void nc_update_payload_path(const char *version, wchar_t *out, size_t cap);
void nc_update_cleanup_old_files(void);
