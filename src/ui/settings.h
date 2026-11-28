#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config/config.h"
#include "ui/ui_host.h"

typedef struct {
    uint32_t modifiers;
    int32_t key;
    int32_t max_run_minutes;
    int32_t max_queue_delay_ms;
    bool auto_update;
    char *update_feed_url;
} NcSettingsOutcome;

bool nc_settings_dialog_run(const NcUiHost *host, const NcConfig *config, NcSettingsOutcome *out);
