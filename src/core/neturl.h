#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core/err.h"

typedef struct {
    char scheme[8];
    char host[256];
    char path[2048];
    int port; /* 0 means scheme default */
} NcUrl;

bool nc_url_parse(const char *text, const char *label, NcUrl *out, NcErr *err);
bool nc_url_validate_feed(const char *text, NcErr *err);
bool nc_url_validate_installer(const char *text, NcErr *err);
bool nc_url_join(const NcUrl *base, const char *location, NcUrl *out, NcErr *err);
bool nc_url_validate_redirect(const NcUrl *current, const NcUrl *next, NcErr *err);
bool nc_url_validate_public_target(const NcUrl *url, bool allow_loopback, NcErr *err);
bool nc_url_is_loopback(const NcUrl *url);
bool nc_url_redact(const char *url, char *out, size_t cap);
bool nc_neturl_normalize_sha256(const char *value, char out[65]);
bool nc_ip_text_is_global(const char *text);
bool nc_ip_text_is_loopback(const char *text);
int nc_url_default_port(const NcUrl *url);
