#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "config/config.h"
#include "core/err.h"
#include "core/hash.h"
#include "core/version.h"
#include "update/update_log.h"

typedef struct {
    char *url;
    char *sha256;
    int64_t size;
} NcUpdateInstaller;

typedef struct {
    int32_t schema_version;
    char *version;
    char *published_at;
    char *notes;
    char *page_url;
    bool has_installer;
    NcUpdateInstaller installer;
} NcUpdateManifest;

typedef enum {
    NC_UPDATE_CHECK_UP_TO_DATE = 0,
    NC_UPDATE_CHECK_AVAILABLE,
    NC_UPDATE_CHECK_NOT_CONFIGURED,
    NC_UPDATE_CHECK_FAILED
} NcUpdateCheckKind;

typedef struct {
    NcUpdateCheckKind kind;
    char *error;
    NcUpdateManifest manifest;
    NcVersion version;
} NcUpdateCheckResult;

void nc_update_manifest_free(NcUpdateManifest *manifest);
bool nc_update_parse_manifest(const char *json, size_t length, NcUpdateManifest *out,
                              NcVersion *version, NcErr *err);
NcUpdateCheckResult nc_update_check(const char *feed_url, const NcVersion *current);
void nc_update_check_result_free(NcUpdateCheckResult *result);
bool nc_update_download(const NcUpdateManifest *manifest, const wchar_t *destination, NcErr *err);
bool nc_update_apply_self_update(const wchar_t *new_file);
