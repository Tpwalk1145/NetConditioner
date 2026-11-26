#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "neturl.h"
#include "version.h"

static char *dup_or_empty(const char *text) {
    return nc_strdup(text ? text : "");
}

void nc_config_init(NcConfig *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->version = 1;
    config->hotkey.modifiers = NC_DEFAULT_HOTKEY_MODIFIERS;
    config->hotkey.key = NC_DEFAULT_HOTKEY_KEY;
    config->settings.max_run_minutes = 0;
    config->settings.max_queue_delay_ms = 2000;
    nc_string_list_init(&config->settings.process_names);
    config->settings.process_names.count = 0;
    config->settings.update_feed_url = dup_or_empty("");
    config->settings.auto_update = true;
    config->settings.last_attempted_version = dup_or_empty("");
}

void nc_config_defaults(NcConfig *config) {
    nc_config_init(config);
}

void nc_config_fresh(NcConfig *config) {
    nc_config_init(config);
    nc_config_ensure_default_profile(config);
}

void nc_config_free_profile(NcProfile *profile) {
    if (!profile) {
        return;
    }
    free(profile->id);
    free(profile->name);
    profile->id = NULL;
    profile->name = NULL;
    memset(&profile->uplink, 0, sizeof(profile->uplink));
    memset(&profile->downlink, 0, sizeof(profile->downlink));
    profile->protocol = NC_PROTOCOL_ALL;
}

void nc_config_free(NcConfig *config) {
    size_t index;
    if (!config) {
        return;
    }
    free(config->active_profile_id);
    for (index = 0; index < config->profile_count; index++) {
        nc_config_free_profile(&config->profiles[index]);
    }
    free(config->profiles);
    nc_string_list_free(&config->settings.process_names);
    free(config->settings.update_feed_url);
    free(config->settings.last_attempted_version);
    memset(config, 0, sizeof(*config));
}

char *nc_config_new_id(void) {
    char id[33];
    nc_new_id(id);
    return nc_strdup(id);
}

const char *nc_protocol_name(NcConditionProtocol protocol) {
    switch (protocol) {
    case NC_PROTOCOL_TCP:
        return "Tcp";
    case NC_PROTOCOL_UDP:
        return "Udp";
    case NC_PROTOCOL_ALL:
    default:
        return "All";
    }
}

bool nc_protocol_from_name(const char *text, NcConditionProtocol *out) {
    if (!text || !out) {
        return false;
    }
    if (nc_ieq_ascii(text, "Tcp")) {
        *out = NC_PROTOCOL_TCP;
        return true;
    }
    if (nc_ieq_ascii(text, "Udp")) {
        *out = NC_PROTOCOL_UDP;
        return true;
    }
    if (nc_ieq_ascii(text, "All")) {
        *out = NC_PROTOCOL_ALL;
        return true;
    }
    return false;
}

bool nc_config_copy_profile(const NcProfile *source, NcProfile *target) {
    if (!source || !target) {
        return false;
    }
    memset(target, 0, sizeof(*target));
    target->id = dup_or_empty(source->id);
    target->name = dup_or_empty(source->name);
    if (!target->id || !target->name) {
        free(target->id);
        free(target->name);
        target->id = NULL;
        target->name = NULL;
        return false;
    }
    target->uplink = source->uplink;
    target->downlink = source->downlink;
    target->protocol = source->protocol;
    return true;
}

static bool profiles_reserve(NcConfig *config, size_t extra) {
    size_t need;
    size_t cap;
    NcProfile *profiles;
    if (!config || extra > SIZE_MAX - config->profile_count) {
        return false;
    }
    need = config->profile_count + extra;
    if (need <= config->profile_cap) {
        return true;
    }
    cap = config->profile_cap ? config->profile_cap : 4;
    while (cap < need) {
        cap *= 2;
    }
    profiles = (NcProfile *)realloc(config->profiles, cap * sizeof(NcProfile));
    if (!profiles) {
        return false;
    }
    config->profiles = profiles;
    config->profile_cap = cap;
    return true;
}

bool nc_config_add_profile(NcConfig *config, const NcProfile *profile) {
    NcProfile copy;
    if (!config || !profile) {
        return false;
    }
    if (!nc_config_copy_profile(profile, &copy)) {
        return false;
    }
    if (!profiles_reserve(config, 1)) {
        nc_config_free_profile(&copy);
        return false;
    }
    config->profiles[config->profile_count++] = copy;
    return true;
}

bool nc_config_replace_profile(NcConfig *config, size_t index, const NcProfile *profile) {
    NcProfile copy;
    if (!config || !profile || index >= config->profile_count) {
        return false;
    }
    if (!nc_config_copy_profile(profile, &copy)) {
        return false;
    }
    nc_config_free_profile(&config->profiles[index]);
    config->profiles[index] = copy;
    return true;
}

void nc_config_clear_profiles(NcConfig *config) {
    size_t index;
    if (!config) {
        return;
    }
    for (index = 0; index < config->profile_count; index++) {
        nc_config_free_profile(&config->profiles[index]);
    }
    config->profile_count = 0;
}

size_t nc_config_find_profile_index(const NcConfig *config, const char *id) {
    size_t index;
    if (!config || !id) {
        return (size_t)-1;
    }
    for (index = 0; index < config->profile_count; index++) {
        if (config->profiles[index].id && strcmp(config->profiles[index].id, id) == 0) {
            return index;
        }
    }
    return (size_t)-1;
}

NcProfile *nc_config_find_profile(NcConfig *config, const char *id) {
    size_t index = nc_config_find_profile_index(config, id);
    return index == (size_t)-1 ? NULL : &config->profiles[index];
}

NcProfile *nc_config_active_profile(NcConfig *config) {
    size_t index;
    if (!config || config->profile_count == 0) {
        return NULL;
    }
    index = nc_config_find_profile_index(config, config->active_profile_id);
    if (index == (size_t)-1) {
        index = 0;
    }
    return &config->profiles[index];
}

void nc_config_ensure_default_profile(NcConfig *config) {
    if (!config) {
        return;
    }
    if (config->profile_count == 0) {
        NcProfile profile;
        memset(&profile, 0, sizeof(profile));
        profile.id = nc_config_new_id();
        profile.name = nc_strdup("默认配置");
        profile.protocol = NC_PROTOCOL_ALL;
        if (profile.id && profile.name) {
            nc_config_add_profile(config, &profile);
        }
        nc_config_free_profile(&profile);
    }
    if (config->profile_count > 0) {
        size_t active = nc_config_find_profile_index(config, config->active_profile_id);
        if (active == (size_t)-1) {
            free(config->active_profile_id);
            config->active_profile_id = nc_strdup(config->profiles[0].id ? config->profiles[0].id : "");
        }
    }
}

void nc_config_delete_profile(NcConfig *config, const char *id) {
    size_t index;
    if (!config || !id) {
        return;
    }
    index = nc_config_find_profile_index(config, id);
    if (index == (size_t)-1) {
        return;
    }
    {
        bool was_active = config->active_profile_id && strcmp(config->active_profile_id, id) == 0;
        nc_config_free_profile(&config->profiles[index]);
        memmove(&config->profiles[index], &config->profiles[index + 1],
                (config->profile_count - index - 1) * sizeof(NcProfile));
        config->profile_count--;
        if (config->profile_count == 0) {
            NcProfile profile;
            memset(&profile, 0, sizeof(profile));
            profile.id = nc_config_new_id();
            profile.name = nc_strdup("默认配置");
            profile.protocol = NC_PROTOCOL_ALL;
            if (profile.id && profile.name) {
                nc_config_add_profile(config, &profile);
            }
            nc_config_free_profile(&profile);
            free(config->active_profile_id);
            config->active_profile_id = nc_strdup(config->profiles[0].id);
        } else if (was_active) {
            free(config->active_profile_id);
            config->active_profile_id = nc_strdup(config->profiles[0].id);
        }
    }
}

bool nc_direction_all_zero(const NcDirectionProfile *direction) {
    if (!direction) {
        return true;
    }
    return direction->delay_ms == 0 && direction->jitter_ms == 0 &&
           direction->bandwidth_kbps == 0 && direction->random_loss_percent == 0 &&
           direction->burst_allow_ms == 0 && direction->burst_drop_ms == 0;
}

void nc_direction_clamp(NcDirectionProfile *direction) {
    if (!direction) {
        return;
    }
    if (direction->delay_ms < 0) direction->delay_ms = 0;
    if (direction->delay_ms > 60000) direction->delay_ms = 60000;
    if (direction->jitter_ms < 0) direction->jitter_ms = 0;
    if (direction->jitter_ms > 60000) direction->jitter_ms = 60000;
    if (direction->bandwidth_kbps < 0) direction->bandwidth_kbps = 0;
    if (direction->bandwidth_kbps > 1000000) direction->bandwidth_kbps = 1000000;
    if (direction->random_loss_percent < 0) direction->random_loss_percent = 0;
    if (direction->random_loss_percent > 100) direction->random_loss_percent = 100;
    if (direction->burst_allow_ms < 0) direction->burst_allow_ms = 0;
    if (direction->burst_allow_ms > 600000) direction->burst_allow_ms = 600000;
    if (direction->burst_drop_ms < 0) direction->burst_drop_ms = 0;
    if (direction->burst_drop_ms > 600000) direction->burst_drop_ms = 600000;
}

bool nc_direction_validate(const NcDirectionProfile *direction, const char *direction_name, NcErr *err) {
    const char *name = direction_name ? direction_name : "";
    if (!direction) {
        return nc_fail(err, "%s参数无效。", name);
    }
    if (direction->delay_ms < 0 || direction->delay_ms > 60000) {
        return nc_fail(err, "%s延时必须在 0 到 60000 ms 之间。", name);
    }
    if (direction->jitter_ms < 0 || direction->jitter_ms > 60000) {
        return nc_fail(err, "%s延时抖动必须在 0 到 60000 ms 之间。", name);
    }
    if (direction->bandwidth_kbps < 0 || direction->bandwidth_kbps > 1000000) {
        return nc_fail(err, "%s带宽必须在 0 到 1000000 kbps 之间。", name);
    }
    if (direction->random_loss_percent < 0 || direction->random_loss_percent > 100) {
        return nc_fail(err, "%s随机丢包必须在 0 到 100% 之间。", name);
    }
    if (direction->burst_allow_ms < 0 || direction->burst_allow_ms > 600000) {
        return nc_fail(err, "%s连续丢包-放行必须在 0 到 600000 ms 之间。", name);
    }
    if (direction->burst_drop_ms < 0 || direction->burst_drop_ms > 600000) {
        return nc_fail(err, "%s连续丢包-丢包必须在 0 到 600000 ms 之间。", name);
    }
    return true;
}

bool nc_profile_all_zero(const NcProfile *profile) {
    return profile && nc_direction_all_zero(&profile->uplink) && nc_direction_all_zero(&profile->downlink);
}

size_t nc_config_utf8_char_count(const char *text) {
    size_t count = 0;
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    while (*cursor) {
        if ((*cursor & 0xC0) != 0x80) {
            count++;
        }
        cursor++;
    }
    return count;
}

bool nc_profile_validate(const NcProfile *profile, NcErr *err) {
    char *trimmed;
    size_t length;
    bool ok;
    if (!profile) {
        return nc_fail(err, "配置参数无效。");
    }
    trimmed = nc_strdup(profile->name ? profile->name : "");
    if (!trimmed) {
        return nc_fail(err, "内存不足。");
    }
    nc_trim(trimmed);
    if (trimmed[0] == 0) {
        free(trimmed);
        return nc_fail(err, "配置名称不能为空。");
    }
    length = nc_config_utf8_char_count(trimmed);
    free(trimmed);
    if (length > 60) {
        return nc_fail(err, "配置名称不能超过 60 个字符。");
    }
    if (!nc_direction_validate(&profile->uplink, "上行", err)) {
        return false;
    }
    ok = nc_direction_validate(&profile->downlink, "下行", err);
    return ok;
}

void nc_config_normalize_process_name(const char *raw, Str *out) {
    nc_normalize_process_name(raw, out);
}

void nc_config_normalize_display_name(const char *raw, Str *out) {
    nc_normalize_display_name(raw, out);
}

void nc_config_sanitize_process_names(NcStringList *names) {
    NcStringList result;
    size_t index;
    if (!names) {
        return;
    }
    nc_string_list_init(&result);
    for (index = 0; index < names->count; index++) {
        Str display;
        str_init(&display);
        nc_normalize_display_name(names->items[index], &display);
        if (display.len > 0 && !nc_string_list_contains_ci(&result, str_cstr(&display))) {
            nc_string_list_push(&result, str_cstr(&display));
        }
        str_free(&display);
        if (result.count >= NC_MAX_PROCESS_COUNT) {
            break;
        }
    }
    nc_string_list_free(names);
    *names = result;
}

static void read_direction(const JsonValue *object, NcDirectionProfile *out) {
    memset(out, 0, sizeof(*out));
    if (!object || object->type != JSON_OBJECT) {
        return;
    }
    json_get_i32(object, "delayMs", &out->delay_ms);
    json_get_i32(object, "jitterMs", &out->jitter_ms);
    json_get_i32(object, "bandwidthKbps", &out->bandwidth_kbps);
    json_get_i32(object, "randomLossPercent", &out->random_loss_percent);
    json_get_i32(object, "burstAllowMs", &out->burst_allow_ms);
    json_get_i32(object, "burstDropMs", &out->burst_drop_ms);
}

static bool read_profile(const JsonValue *object, NcProfile *out) {
    JsonValue *value;
    if (!object || object->type != JSON_OBJECT) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->protocol = NC_PROTOCOL_ALL;
    value = json_object_get(object, "id");
    out->id = dup_or_empty(value && value->type == JSON_STRING ? value->u.string : "");
    value = json_object_get(object, "name");
    if (!value) {
        out->name = nc_strdup("默认配置");
    } else if (value->type == JSON_STRING) {
        out->name = nc_strdup(value->u.string);
    } else {
        out->name = nc_strdup("");
    }
    read_direction(json_object_get(object, "uplink"), &out->uplink);
    read_direction(json_object_get(object, "downlink"), &out->downlink);
    value = json_object_get(object, "protocol");
    if (value && value->type == JSON_STRING) {
        NcConditionProtocol protocol;
        if (nc_protocol_from_name(value->u.string, &protocol)) {
            out->protocol = protocol;
        }
    }
    if (!out->id || !out->name) {
        nc_config_free_profile(out);
        return false;
    }
    return true;
}

static void read_settings(const JsonValue *object, NcSettings *out) {
    JsonValue *value;
    size_t index;
    if (!object || object->type != JSON_OBJECT) {
        return;
    }
    if (!json_get_i32(object, "maxRunMinutes", &out->max_run_minutes)) {
        out->max_run_minutes = 0;
    }
    if (!json_get_i32(object, "maxQueueDelayMs", &out->max_queue_delay_ms)) {
        out->max_queue_delay_ms = 2000;
    }
    value = json_get_array(object, "processNames");
    if (value) {
        for (index = 0; index < value->u.array.len; index++) {
            JsonValue *item = value->u.array.items[index];
            if (item->type == JSON_STRING) {
                nc_string_list_push(&out->process_names, item->u.string);
            }
        }
    }
    value = json_object_get(object, "updateFeedUrl");
    free(out->update_feed_url);
    out->update_feed_url = dup_or_empty(value && value->type == JSON_STRING ? value->u.string : "");
    if (!json_get_bool(object, "autoUpdate", &out->auto_update)) {
        out->auto_update = true;
    }
    {
        JsonValue *stamp = json_object_get(object, "lastUpdateCheckUnix");
        out->last_update_check_unix = (stamp && stamp->type == JSON_NUMBER && stamp->u.number.is_int)
                                          ? stamp->u.number.integer
                                          : 0;
    }
    {
        JsonValue *stamp = json_object_get(object, "lastUpdateAttemptUnix");
        out->last_update_attempt_unix = (stamp && stamp->type == JSON_NUMBER && stamp->u.number.is_int)
                                            ? stamp->u.number.integer
                                            : 0;
    }
    value = json_object_get(object, "lastAttemptedVersion");
    free(out->last_attempted_version);
    out->last_attempted_version = dup_or_empty(value && value->type == JSON_STRING ? value->u.string : "");
}

bool nc_config_from_json(const JsonValue *root, NcConfig *out) {
    JsonValue *value;
    if (!out) {
        return false;
    }
    nc_config_defaults(out);
    if (!root || root->type != JSON_OBJECT) {
        return true;
    }
    if (!json_get_i32(root, "version", &out->version)) {
        out->version = 1;
    }
    value = json_object_get(root, "activeProfileId");
    if (value && value->type == JSON_STRING) {
        free(out->active_profile_id);
        out->active_profile_id = nc_strdup(value->u.string);
    }
    value = json_get_array(root, "profiles");
    if (value) {
        size_t index;
        for (index = 0; index < value->u.array.len; index++) {
            JsonValue *item = value->u.array.items[index];
            NcProfile profile;
            if (item->type != JSON_OBJECT) {
                continue;
            }
            if (!read_profile(item, &profile)) {
                continue;
            }
            if (!nc_config_add_profile(out, &profile)) {
                nc_config_free_profile(&profile);
                continue;
            }
            nc_config_free_profile(&profile);
        }
    }
    value = json_get_object(root, "hotkey");
    if (value) {
        JsonValue *modifiers = json_object_get(value, "modifiers");
        JsonValue *key = json_object_get(value, "key");
        if (modifiers && modifiers->type == JSON_NUMBER && modifiers->u.number.is_int &&
            modifiers->u.number.integer >= 0 && modifiers->u.number.integer <= UINT32_MAX) {
            out->hotkey.modifiers = (uint32_t)modifiers->u.number.integer;
        }
        if (key && key->type == JSON_NUMBER && key->u.number.is_int &&
            key->u.number.integer >= 0 && key->u.number.integer <= INT32_MAX) {
            out->hotkey.key = (int32_t)key->u.number.integer;
        }
    }
    read_settings(json_get_object(root, "settings"), &out->settings);
    return true;
}

JsonValue *nc_config_to_json(const NcConfig *config) {
    JsonValue *root;
    JsonValue *profiles;
    JsonValue *hotkey;
    JsonValue *settings;
    JsonValue *process_names;
    size_t index;
    if (!config) {
        return NULL;
    }
    root = json_object();
    profiles = json_array();
    hotkey = json_object();
    settings = json_object();
    process_names = json_array();
    if (!root || !profiles || !hotkey || !settings || !process_names) {
        json_free(root);
        json_free(profiles);
        json_free(hotkey);
        json_free(settings);
        json_free(process_names);
        return NULL;
    }
    if (config->active_profile_id) {
        json_object_set(root, "activeProfileId", json_string(config->active_profile_id));
    } else {
        json_object_set(root, "activeProfileId", json_null());
    }
    json_object_set(root, "version", json_integer(config->version));
    for (index = 0; index < config->profile_count; index++) {
        const NcProfile *profile = &config->profiles[index];
        JsonValue *item = json_object();
        JsonValue *uplink = json_object();
        JsonValue *downlink = json_object();
        if (!item || !uplink || !downlink) {
            json_free(item);
            json_free(uplink);
            json_free(downlink);
            continue;
        }
        json_object_set(item, "id", json_string(profile->id ? profile->id : ""));
        json_object_set(item, "name", json_string(profile->name ? profile->name : ""));
        json_object_set(item, "protocol", json_string(nc_protocol_name(profile->protocol)));
        json_object_set(uplink, "delayMs", json_integer(profile->uplink.delay_ms));
        json_object_set(uplink, "jitterMs", json_integer(profile->uplink.jitter_ms));
        json_object_set(uplink, "bandwidthKbps", json_integer(profile->uplink.bandwidth_kbps));
        json_object_set(uplink, "randomLossPercent", json_integer(profile->uplink.random_loss_percent));
        json_object_set(uplink, "burstAllowMs", json_integer(profile->uplink.burst_allow_ms));
        json_object_set(uplink, "burstDropMs", json_integer(profile->uplink.burst_drop_ms));
        json_object_set(downlink, "delayMs", json_integer(profile->downlink.delay_ms));
        json_object_set(downlink, "jitterMs", json_integer(profile->downlink.jitter_ms));
        json_object_set(downlink, "bandwidthKbps", json_integer(profile->downlink.bandwidth_kbps));
        json_object_set(downlink, "randomLossPercent", json_integer(profile->downlink.random_loss_percent));
        json_object_set(downlink, "burstAllowMs", json_integer(profile->downlink.burst_allow_ms));
        json_object_set(downlink, "burstDropMs", json_integer(profile->downlink.burst_drop_ms));
        json_object_set(item, "uplink", uplink);
        json_object_set(item, "downlink", downlink);
        json_array_push(profiles, item);
    }
    json_object_set(root, "profiles", profiles);
    json_object_set(hotkey, "modifiers", json_integer(config->hotkey.modifiers));
    json_object_set(hotkey, "key", json_integer(config->hotkey.key));
    json_object_set(root, "hotkey", hotkey);
    for (index = 0; index < config->settings.process_names.count; index++) {
        json_array_push(process_names, json_string(config->settings.process_names.items[index]));
    }
    json_object_set(settings, "maxRunMinutes", json_integer(config->settings.max_run_minutes));
    json_object_set(settings, "maxQueueDelayMs", json_integer(config->settings.max_queue_delay_ms));
    json_object_set(settings, "processNames", process_names);
    json_object_set(settings, "updateFeedUrl", json_string(config->settings.update_feed_url ? config->settings.update_feed_url : ""));
    json_object_set(settings, "autoUpdate", json_bool(config->settings.auto_update));
    json_object_set(settings, "lastUpdateCheckUnix", json_integer(config->settings.last_update_check_unix));
    json_object_set(settings, "lastUpdateAttemptUnix", json_integer(config->settings.last_update_attempt_unix));
    json_object_set(settings, "lastAttemptedVersion", json_string(config->settings.last_attempted_version ? config->settings.last_attempted_version : ""));
    json_object_set(root, "settings", settings);
    return root;
}

static int64_t clamp_unix(int64_t value, int64_t now) {
    if (value < 0) {
        return 0;
    }
    if (value > now + 86400) {
        return now;
    }
    return value;
}

static void sanitize_feed_url(char **field) {
    NcErr error;
    if (!field) {
        return;
    }
    if (!*field) {
        *field = nc_strdup("");
        return;
    }
    nc_trim(*field);
    if ((*field)[0] == 0 || strlen(*field) > 2048 || !nc_url_validate_feed(*field, &error)) {
        free(*field);
        *field = nc_strdup("");
    }
}

static void lowercase_ascii(char *text) {
    if (!text) {
        return;
    }
    while (*text) {
        *text = (char)tolower((unsigned char)*text);
        text++;
    }
}

void nc_config_sanitize(NcConfig *config, int64_t now_unix, size_t *removed_profiles) {
    NcProfile *kept;
    size_t kept_count = 0;
    size_t index;
    size_t removed = 0;
    if (!config) {
        return;
    }
    config->hotkey.modifiers &= (NC_MOD_ALT | NC_MOD_CONTROL | NC_MOD_SHIFT | NC_MOD_WIN);
    if (config->hotkey.modifiers == 0) {
        config->hotkey.modifiers = NC_DEFAULT_HOTKEY_MODIFIERS;
    }
    if (config->hotkey.key < 1 || config->hotkey.key > 0xFF) {
        config->hotkey.key = NC_DEFAULT_HOTKEY_KEY;
    }
    if (config->settings.max_run_minutes < 0) config->settings.max_run_minutes = 0;
    if (config->settings.max_run_minutes > 1440) config->settings.max_run_minutes = 1440;
    if (config->settings.max_queue_delay_ms < 0) config->settings.max_queue_delay_ms = 0;
    if (config->settings.max_queue_delay_ms > 60000) config->settings.max_queue_delay_ms = 60000;
    nc_config_sanitize_process_names(&config->settings.process_names);
    if (!config->settings.update_feed_url) {
        config->settings.update_feed_url = nc_strdup("");
    }
    sanitize_feed_url(&config->settings.update_feed_url);
    config->settings.last_update_check_unix = clamp_unix(config->settings.last_update_check_unix, now_unix);
    config->settings.last_update_attempt_unix = clamp_unix(config->settings.last_update_attempt_unix, now_unix);
    if (!config->settings.last_attempted_version) {
        config->settings.last_attempted_version = nc_strdup("");
    } else {
        char normalized[32];
        if (nc_version_normalize(config->settings.last_attempted_version, normalized, sizeof(normalized))) {
            char *copy = nc_strdup(normalized);
            free(config->settings.last_attempted_version);
            config->settings.last_attempted_version = copy;
        } else {
            free(config->settings.last_attempted_version);
            config->settings.last_attempted_version = nc_strdup("");
        }
    }
    kept = config->profile_count ? (NcProfile *)calloc(config->profile_count, sizeof(NcProfile)) : NULL;
    if (!kept && config->profile_count > 0) {
        if (removed_profiles) {
            *removed_profiles = 0;
        }
        return;
    }
    for (index = 0; index < config->profile_count; index++) {
        NcProfile *profile = &config->profiles[index];
        size_t seen;
        bool duplicate = false;
        if (!profile->name) {
            removed++;
            nc_config_free_profile(profile);
            continue;
        }
        nc_trim(profile->name);
        if (profile->name[0] == 0) {
            removed++;
            nc_config_free_profile(profile);
            continue;
        }
        lowercase_ascii(profile->id);
        if (!profile->id || profile->id[0] == 0) {
            free(profile->id);
            profile->id = nc_config_new_id();
        } else {
            for (seen = 0; seen < kept_count; seen++) {
                if (kept[seen].id && strcmp(kept[seen].id, profile->id) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                free(profile->id);
                profile->id = nc_config_new_id();
            }
        }
        nc_direction_clamp(&profile->uplink);
        nc_direction_clamp(&profile->downlink);
        kept[kept_count++] = *profile;
        memset(profile, 0, sizeof(*profile));
    }
    free(config->profiles);
    config->profiles = kept;
    config->profile_count = kept_count;
    config->profile_cap = kept_count;
    nc_config_ensure_default_profile(config);
    if (removed_profiles) {
        *removed_profiles = removed;
    }
}