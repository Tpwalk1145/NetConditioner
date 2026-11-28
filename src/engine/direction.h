#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config.h"
#include "engine/events.h"
#include "engine/flow.h"
#include "engine/process_filter.h"
#include "engine/scheduler.h"

typedef struct NcDirectionEngine NcDirectionEngine;

NcDirectionEngine *nc_direction_new(const char *display_name, const char *filter, bool outbound,
                                    const NcDirectionProfile *profile,
                                    NcConditionProtocol protocol,
                                    NcProcessFlowFilter *process_filter,
                                    int32_t max_queue_delay_ms, NcEngineEvents *events);
bool nc_direction_start(NcDirectionEngine *engine, NcErr *err);
void nc_direction_stop(NcDirectionEngine *engine);
void nc_direction_free(NcDirectionEngine *engine);
void nc_direction_stats(const NcDirectionEngine *engine, NcStatsSnapshot *out);
void nc_direction_reset_stats(NcDirectionEngine *engine);
