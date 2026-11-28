#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "core/err.h"

/* Fast non-cryptographic FNV-1a 64-bit hashes. */
uint64_t nc_fnv1a_bytes(const void *data, size_t size);
uint64_t nc_fnv1a_utf8(const char *text);
uint64_t nc_fnv1a_wide(const wchar_t *text);

/* Cryptographic SHA-256 via Windows CNG (uppercase 64-hex output). */
void nc_sha256_hex(const void *data, size_t length, char out[65]);
bool nc_sha256_file(const wchar_t *path, uint64_t *size, char out[65], NcErr *err);
