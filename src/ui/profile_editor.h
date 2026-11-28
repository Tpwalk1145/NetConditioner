#pragma once

#include <stdbool.h>

#include "config/config.h"
#include "ui/ui_host.h"

typedef enum {
    NC_EDITOR_OUTCOME_NONE = 0,
    NC_EDITOR_OUTCOME_SAVED,
    NC_EDITOR_OUTCOME_DELETED
} NcEditorOutcomeKind;

typedef struct {
    NcEditorOutcomeKind kind;
    NcProfile profile;
} NcEditorOutcome;

bool nc_profile_editor_run(const NcUiHost *host, const NcProfile *existing, bool allow_delete,
                           const NcStringList *existing_names, NcEditorOutcome *out);
