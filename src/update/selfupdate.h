#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>

#include "util.h"

bool nc_selfupdate_is_helper_mode(int argc, wchar_t **argv);
int nc_selfupdate_run_helper(int argc, wchar_t **argv);
bool nc_selfupdate_spawn_helper(const wchar_t *new_file, NcErr *err);
void nc_selfupdate_remove_zone_identifier(const wchar_t *path);
