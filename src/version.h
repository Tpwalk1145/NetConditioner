#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t parts[4];
} NcVersion;

bool nc_version_parse(const char *text, NcVersion *out);
void nc_version_format(const NcVersion *version, char *out, size_t cap);
bool nc_version_normalize(const char *text, char *out, size_t cap);
bool nc_version_is_newer(const NcVersion *current, const NcVersion *candidate);
int nc_version_compare(const NcVersion *left, const NcVersion *right);
NcVersion nc_version_current(void);
void nc_version_display(char *out, size_t cap);
