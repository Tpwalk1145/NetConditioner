#include "core/err.h"

#include <stdio.h>

void nc_err_vset(NcErr *err, const char *fmt, va_list ap) {
    if (!err) {
        return;
    }
    if (!fmt) {
        err->msg[0] = 0;
        return;
    }
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    err->msg[sizeof(err->msg) - 1] = 0;
}

void nc_err_set(NcErr *err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nc_err_vset(err, fmt, ap);
    va_end(ap);
}

bool nc_fail(NcErr *err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nc_err_vset(err, fmt, ap);
    va_end(ap);
    return false;
}
