#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

#include "flow.h"
#include "util.h"

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

typedef struct NcProcessFlowFilter NcProcessFlowFilter;

NcProcessFlowFilter *nc_process_filter_new(const char *const *names, size_t count);
void nc_process_filter_free(NcProcessFlowFilter *filter);
void nc_process_filter_shutdown(NcProcessFlowFilter *filter);
bool nc_process_filter_match(NcProcessFlowFilter *filter, uint8_t protocol,
                             const NcFlowEndpoint *local, const NcFlowEndpoint *remote);
