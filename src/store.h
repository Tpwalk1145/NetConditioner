#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "util.h"

typedef struct {
    wchar_t path[32768];
} NcStore;

bool nc_store_default_path(wchar_t *out, size_t cap);
void nc_store_init(NcStore *store);
/* Loads a usable configuration; warning (may be NULL) receives a user-facing message. */
bool nc_store_load(NcStore *store, NcConfig *out, char *warning, size_t warning_cap);
bool nc_store_save(NcStore *store, const NcConfig *config, NcErr *err);
