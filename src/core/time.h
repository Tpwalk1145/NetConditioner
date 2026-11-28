#pragma once

#include <stddef.h>
#include <stdint.h>

int64_t nc_now_unix(void);
void nc_format_local_iso_ms(char *buf, size_t cap);
void nc_format_local_hms_ms(char *buf, size_t cap);
void nc_format_local_hms(char *buf, size_t cap);
uint64_t nc_monotonic_ns(void);

uint64_t nc_random_seed(void);
uint64_t nc_rand_next(uint64_t *state);
uint32_t nc_rand_below(uint64_t *state, uint32_t bound);
uint64_t nc_rand_inclusive(uint64_t *state, uint64_t max);
void nc_new_id(char out[33]);
