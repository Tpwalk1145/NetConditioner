#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "update.h"
#include "version.h"

typedef struct {
    int64_t last_update_attempt_unix;
    char *last_attempted_version;
} NcUpdateSettingsSnapshot;

typedef enum {
    NC_TICK_CHECK_FAILED = 0,
    NC_TICK_UP_TO_DATE,
    NC_TICK_READY,
    NC_TICK_DOWNLOAD_FAILED
} NcTickKind;

typedef struct {
    NcTickKind kind;
    char *error;
    NcUpdateManifest manifest;
    wchar_t installer[32768];
    int64_t now;
} NcTickOutcome;

void nc_tick_outcome_free(NcTickOutcome *outcome);
bool nc_should_check(const NcSettings *settings, int64_t now);
void nc_coordinator_tick(const char *feed_url, const NcVersion *current,
                         const NcUpdateSettingsSnapshot *snapshot, NcTickOutcome *outcome);
