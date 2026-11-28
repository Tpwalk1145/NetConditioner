#pragma once

#include <stdarg.h>
#include <stdbool.h>

static_assert(__STDC_VERSION__ >= 202311L, "NetConditioner requires a C23 compiler");

#define NC_ERR_CAP 1024

typedef struct {
    char msg[NC_ERR_CAP];
} NcErr;

void nc_err_vset(NcErr *err, const char *fmt, va_list ap);
void nc_err_set(NcErr *err, const char *fmt, ...);
bool nc_fail(NcErr *err, const char *fmt, ...);
