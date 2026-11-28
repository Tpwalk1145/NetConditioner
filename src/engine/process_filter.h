#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/flow.h"

typedef struct {
    char *name;
    char *path;
} NcProcessInfo;

typedef struct {
    NcProcessInfo *items;
    size_t count;
    size_t cap;
} NcProcessList;

bool nc_process_enumerate(NcProcessList *list);
void nc_process_list_free(NcProcessList *list);

/* Bounded-probe open-addressing negative cache with tombstones and eviction. */
typedef struct {
    uint8_t protocol;
    bool has_remote;
    NcFlowEndpoint local;
    NcFlowEndpoint remote;
} NcNegativeKey;

typedef struct NcNegativeCache NcNegativeCache;

NcNegativeCache *nc_negative_cache_new(size_t capacity);
void nc_negative_cache_free(NcNegativeCache *cache);
bool nc_negative_cache_contains(NcNegativeCache *cache, const NcNegativeKey *key, uint64_t now_ns);
void nc_negative_cache_insert(NcNegativeCache *cache, const NcNegativeKey *key,
                              uint64_t now_ns, uint64_t ttl_ns);

typedef struct NcProcessFlowFilter NcProcessFlowFilter;

NcProcessFlowFilter *nc_process_filter_new(const char *const *names, size_t count);
void nc_process_filter_free(NcProcessFlowFilter *filter);
void nc_process_filter_shutdown(NcProcessFlowFilter *filter);
bool nc_process_filter_refresh_now(NcProcessFlowFilter *filter);
bool nc_process_filter_match(NcProcessFlowFilter *filter, uint8_t protocol,
                             const NcFlowEndpoint *local, const NcFlowEndpoint *remote);
