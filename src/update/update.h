#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "version.h"

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

bool nc_update_directory(wchar_t *out, size_t cap);
bool nc_update_ensure_directory(wchar_t *out, size_t cap, NcErr *err);
void nc_update_log(const char *message);
bool nc_update_is_dry_run(void);
void nc_update_payload_path(const char *version, wchar_t *out, size_t cap);
bool nc_update_apply_self_update(const wchar_t *new_file);
void nc_update_cleanup_old_files(void);

void nc_sha256_hex(const void *data, size_t length, char out[65]);
bool nc_sha256_file(const wchar_t *path, uint64_t *size, char out[65], NcErr *err);
