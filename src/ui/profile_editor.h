#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>

#include "config.h"
#include "ui/widgets.h"

typedef struct NcApp NcApp;
typedef struct NcProfileEditor NcProfileEditor;

typedef enum {
    NC_EDITOR_OUTCOME_NONE = 0,
    NC_EDITOR_OUTCOME_SAVED,
    NC_EDITOR_OUTCOME_DELETED
} NcEditorOutcomeKind;

typedef struct {
    NcEditorOutcomeKind kind;
    NcProfile profile;
} NcEditorOutcome;

NcProfileEditor *nc_profile_editor_create(NcApp *app, const NcProfile *existing, bool allow_delete,
                                          const NcStringList *existing_names);
void nc_profile_editor_free(NcProfileEditor *editor);
HWND nc_profile_editor_hwnd(const NcProfileEditor *editor);
const NcFonts *nc_profile_editor_fonts(const NcProfileEditor *editor);
bool nc_profile_editor_closed(const NcProfileEditor *editor);
bool nc_profile_editor_message(NcProfileEditor *editor, NcApp *app, UINT message, WPARAM wparam,
                               LPARAM lparam, LRESULT *result);
bool nc_profile_editor_take_outcome(NcProfileEditor *editor, NcEditorOutcome *out);
