#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "config/config.h"
#include "core/err.h"
#include "core/str.h"

typedef struct {
    WStr path;
} NcStore;

bool nc_store_default_path_w(WStr *out);
bool nc_store_default_path(wchar_t *out, size_t cap);
void nc_store_init(NcStore *store);
void nc_store_init_path(NcStore *store, const wchar_t *path);
void nc_store_free(NcStore *store);
const wchar_t *nc_store_path(const NcStore *store);
/* Loads a usable configuration; warning (may be NULL) receives a user-facing message. */
bool nc_store_load(NcStore *store, NcConfig *out, char *warning, size_t warning_cap);
bool nc_store_save(NcStore *store, const NcConfig *config, NcErr *err);
