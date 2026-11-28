#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config.h"
#include "engine/events.h"

typedef struct NcNetworkConditioner NcNetworkConditioner;

extern const char *const NC_DEFAULT_OUTBOUND_FILTER;
extern const char *const NC_DEFAULT_INBOUND_FILTER;

NcNetworkConditioner *nc_conditioner_new(NcEngineEvents *events);
void nc_conditioner_free(NcNetworkConditioner *conditioner);
bool nc_conditioner_start(NcNetworkConditioner *conditioner, const NcProfile *profile,
                          const char *outbound_filter, const char *inbound_filter,
                          int32_t max_queue_delay_ms, const char *const *process_names,
                          size_t process_count, NcErr *err);
void nc_conditioner_stop(NcNetworkConditioner *conditioner);
bool nc_conditioner_uplink_stats(NcNetworkConditioner *conditioner, NcStatsSnapshot *out);
bool nc_conditioner_downlink_stats(NcNetworkConditioner *conditioner, NcStatsSnapshot *out);
void nc_conditioner_reset_stats(NcNetworkConditioner *conditioner);
