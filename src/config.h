#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "hotkey.h"
#include "json.h"
#include "util.h"

#define NC_MAX_PROCESS_COUNT 64
#define NC_CONFIG_MAX_BYTES (1024u * 1024u)

typedef enum {
    NC_PROTOCOL_TCP = 0,
    NC_PROTOCOL_UDP = 1,
    NC_PROTOCOL_ALL = 2
} NcConditionProtocol;

typedef struct {
    int32_t delay_ms;
    int32_t jitter_ms;
    int32_t bandwidth_kbps;
    int32_t random_loss_percent;
    int32_t burst_allow_ms;
    int32_t burst_drop_ms;
} NcDirectionProfile;

typedef struct {
    char *id;
    char *name;
    NcDirectionProfile uplink;
    NcDirectionProfile downlink;
    NcConditionProtocol protocol;
} NcProfile;

typedef struct {
    uint32_t modifiers;
    int32_t key;
} NcHotkey;

typedef struct {
    int32_t max_run_minutes;
    int32_t max_queue_delay_ms;
    NcStringList process_names;
    char *update_feed_url;
    bool auto_update;
    int64_t last_update_check_unix;
    int64_t last_update_attempt_unix;
    char *last_attempted_version;
} NcSettings;

typedef struct {
    int32_t version;
    char *active_profile_id;
    NcProfile *profiles;
    size_t profile_count;
    size_t profile_cap;
    NcHotkey hotkey;
    NcSettings settings;
} NcConfig;

void nc_config_init(NcConfig *config);
void nc_config_defaults(NcConfig *config);
void nc_config_fresh(NcConfig *config);
void nc_config_free(NcConfig *config);
void nc_config_free_profile(NcProfile *profile);
bool nc_config_copy_profile(const NcProfile *source, NcProfile *target);

bool nc_config_from_json(const JsonValue *root, NcConfig *out);
JsonValue *nc_config_to_json(const NcConfig *config);

void nc_config_sanitize(NcConfig *config, int64_t now_unix, size_t *removed_profiles);
void nc_config_ensure_default_profile(NcConfig *config);
NcProfile *nc_config_active_profile(NcConfig *config);
NcProfile *nc_config_find_profile(NcConfig *config, const char *id);
size_t nc_config_find_profile_index(const NcConfig *config, const char *id);
bool nc_config_add_profile(NcConfig *config, const NcProfile *profile);
bool nc_config_replace_profile(NcConfig *config, size_t index, const NcProfile *profile);
void nc_config_delete_profile(NcConfig *config, const char *id);
void nc_config_clear_profiles(NcConfig *config);
char *nc_config_new_id(void);

bool nc_direction_all_zero(const NcDirectionProfile *direction);
bool nc_direction_validate(const NcDirectionProfile *direction, const char *direction_name, NcErr *err);
void nc_direction_clamp(NcDirectionProfile *direction);
bool nc_profile_all_zero(const NcProfile *profile);
bool nc_profile_validate(const NcProfile *profile, NcErr *err);

void nc_config_normalize_process_name(const char *raw, Str *out);
void nc_config_normalize_display_name(const char *raw, Str *out);
void nc_config_sanitize_process_names(NcStringList *names);
size_t nc_config_utf8_char_count(const char *text);

const char *nc_protocol_name(NcConditionProtocol protocol);
bool nc_protocol_from_name(const char *text, NcConditionProtocol *out);
