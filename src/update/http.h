#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/err.h"

typedef struct {
    int status;
    unsigned char *body;
    size_t body_size;
} NcHttpResponse;

void nc_http_response_free(NcHttpResponse *response);
/* Performs a GET with validated manual redirects and a body size cap. */
bool nc_http_get(const char *url_text, bool https_only, uint32_t timeout_ms, size_t max_body,
                 NcHttpResponse *out, NcErr *err);
