#pragma once

#include <stdbool.h>

#include "core/str.h"
#include "ui/ui_host.h"

bool nc_process_selector_run(const NcUiHost *host, const NcStringList *current_selected,
                             NcStringList *out);
