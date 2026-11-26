#include "test_harness.h"

#include "../src/config.h"
#include "../src/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DOTNET_CONFIG =
    "{\"version\":1,\"activeProfileId\":\"abc\",\"profiles\":["
    "{\"id\":\"p1\",\"name\":\"默认配置\",\"uplink\":{\"delayMs\":300,\"jitterMs\":100,"
    "\"bandwidthKbps\":64,\"randomLossPercent\":50,\"burstAllowMs\":1000,\"burstDropMs\":2000},"
    "\"downlink\":{},\"protocol\":\"Tcp\"},null,"
    "{\"id\":\"p2\",\"name\":\"  \",\"protocol\":\"Udp\"}],"
    "\"hotkey\":{\"modifiers\":3,\"key\":85},"
    "\"settings\":{\"maxRunMinutes\":9999,\"maxQueueDelayMs\":-5,"
    "\"processNames\":[\"Chrome.EXE\",\"C:\\\\Tools\\\\MyApp.exe\",\"chrome.exe\"],"
    "\"updateFeedUrl\":\"http://example.com/latest.json\",\"autoUpdate\":true,"
    "\"lastUpdateCheckUnix\":99999999999999,\"lastAttemptedVersion\":\"1.2\"}}";

static bool load_config_text(const char *text, NcConfig *config, size_t *removed) {
    NcErr error;
    JsonValue *root = json_parse_cstr(text, &error);
    if (!root) {
        return false;
    }
    nc_config_init(config);
    if (!nc_config_from_json(root, config)) {
        json_free(root);
        nc_config_free(config);
        return false;
    }
    json_free(root);
    nc_config_sanitize(config, 1700000000, removed);
    return true;
}

void test_config_run(void) {
    NcConfig config;
    size_t removed = 0;
    NcErr error;

    CHECK(load_config_text(DOTNET_CONFIG, &config, &removed));
    CHECK_INT(removed, 1);
    CHECK_INT(config.profile_count, 1);
    if (config.profile_count == 1) {
        CHECK_STR(config.profiles[0].name, "默认配置");
        CHECK_INT(config.profiles[0].protocol, NC_PROTOCOL_TCP);
        CHECK_INT(config.profiles[0].uplink.delay_ms, 300);
        CHECK_INT(config.profiles[0].uplink.jitter_ms, 100);
        CHECK_INT(config.profiles[0].uplink.bandwidth_kbps, 64);
        CHECK_INT(config.profiles[0].uplink.random_loss_percent, 50);
        CHECK_INT(config.profiles[0].uplink.burst_allow_ms, 1000);
        CHECK_INT(config.profiles[0].uplink.burst_drop_ms, 2000);
    }
    CHECK_STR(config.active_profile_id, "p1");
    CHECK_INT(config.settings.max_run_minutes, 1440);
    CHECK_INT(config.settings.max_queue_delay_ms, 0);
    CHECK_INT(config.settings.process_names.count, 2);
    if (config.settings.process_names.count == 2) {
        CHECK_STR(config.settings.process_names.items[0], "chrome.exe");
        CHECK_STR(config.settings.process_names.items[1], "myapp.exe");
    }
    CHECK_STR(config.settings.update_feed_url, "");
    CHECK(config.settings.last_update_check_unix <= 1700000000);
    CHECK_STR(config.settings.last_attempted_version, "1.2.0");
    nc_config_free(&config);

    CHECK(load_config_text("{\"settings\":null,\"hotkey\":null,\"profiles\":[{\"name\":null}]}",
                           &config, &removed));
    CHECK_INT(removed, 1);
    CHECK_INT(config.profile_count, 1);
    if (config.profile_count == 1) {
        CHECK_STR(config.profiles[0].name, "默认配置");
    }
    CHECK_INT(config.settings.max_queue_delay_ms, 2000);
    CHECK(config.settings.auto_update);
    CHECK_INT(config.hotkey.modifiers, NC_DEFAULT_HOTKEY_MODIFIERS);
    CHECK_INT(config.hotkey.key, NC_DEFAULT_HOTKEY_KEY);
    nc_config_free(&config);

    /* Serialization uses camelCase, sorted keys and full field sets. */
    {
        JsonValue *root;
        Str text;
        nc_config_fresh(&config);
        root = nc_config_to_json(&config);
        CHECK(root != NULL);
        str_init(&text);
        CHECK(json_serialize(root, &text));
        CHECK(strstr(str_cstr(&text), "\"activeProfileId\"") != NULL);
        CHECK(strstr(str_cstr(&text), "\"delayMs\"") != NULL);
        CHECK(strstr(str_cstr(&text), "\"protocol\": \"All\"") != NULL);
        CHECK(strstr(str_cstr(&text), "\"maxQueueDelayMs\": 2000") != NULL);
        CHECK(strstr(str_cstr(&text), "\"autoUpdate\": true") != NULL);
        CHECK(strstr(str_cstr(&text), "\"processNames\"") != NULL);
        {
            const char *active = strstr(str_cstr(&text), "\"activeProfileId\"");
            const char *hotkey = strstr(str_cstr(&text), "\"hotkey\"");
            const char *profiles = strstr(str_cstr(&text), "\"profiles\"");
            const char *settings = strstr(str_cstr(&text), "\"settings\"");
            const char *version = strstr(str_cstr(&text), "\"version\"");
            CHECK(active && hotkey && profiles && settings && version);
            CHECK(active < hotkey && hotkey < profiles && profiles < settings && settings < version);
        }
        str_free(&text);
        json_free(root);
        nc_config_free(&config);
    }

    CHECK(load_config_text("{\"profiles\":[{\"id\":\"x\",\"name\":\"a\"},{\"id\":\"x\",\"name\":\"b\"}]}",
                           &config, &removed));
    CHECK_INT(config.profile_count, 2);
    if (config.profile_count == 2) {
        CHECK(strcmp(config.profiles[0].id, config.profiles[1].id) != 0);
    }
    nc_config_free(&config);

    {
        Str name;
        str_init(&name);
        nc_config_normalize_process_name("应用.EXE", &name);
        CHECK_STR(str_cstr(&name), "应用");
        nc_config_normalize_process_name("ééé.ExE", &name);
        CHECK_STR(str_cstr(&name), "ééé");
        nc_config_normalize_process_name("C:\\工具\\应用.exe", &name);
        CHECK_STR(str_cstr(&name), "应用");
        nc_config_normalize_display_name("ééé", &name);
        CHECK_STR(str_cstr(&name), "ééé.exe");
        nc_config_normalize_process_name("应用", &name);
        CHECK_STR(str_cstr(&name), "应用");
        str_free(&name);
    }

    CHECK(load_config_text(
        "{\"hotkey\":{\"modifiers\":-1,\"key\":0},"
        "\"settings\":{\"maxRunMinutes\":\"bad\",\"processNames\":[\"ok.exe\",3,null]}}",
        &config, &removed));
    CHECK_INT(config.hotkey.modifiers, NC_DEFAULT_HOTKEY_MODIFIERS);
    CHECK_INT(config.hotkey.key, NC_DEFAULT_HOTKEY_KEY);
    CHECK_INT(config.settings.process_names.count, 1);
    if (config.settings.process_names.count == 1) {
        CHECK_STR(config.settings.process_names.items[0], "ok.exe");
    }
    config.hotkey.modifiers = NC_MOD_NOREPEAT;
    config.hotkey.key = 300;
    nc_config_sanitize(&config, 0, &removed);
    CHECK_INT(config.hotkey.modifiers, NC_DEFAULT_HOTKEY_MODIFIERS);
    CHECK_INT(config.hotkey.key, NC_DEFAULT_HOTKEY_KEY);
    nc_config_free(&config);

    CHECK(load_config_text(
        "{\"version\":1,\"future\":7,\"profiles\":[{\"id\":\"p\",\"name\":\"kept\","
        "\"uplink\":{\"delayMs\":\"bad\",\"jitterMs\":9}}],"
        "\"settings\":{\"maxRunMinutes\":\"bad\",\"futureField\":true}}",
        &config, &removed));
    CHECK_INT(config.profile_count, 1);
    if (config.profile_count == 1) {
        CHECK_STR(config.profiles[0].name, "kept");
        CHECK_INT(config.profiles[0].uplink.delay_ms, 0);
        CHECK_INT(config.profiles[0].uplink.jitter_ms, 9);
    }
    CHECK_INT(config.settings.max_run_minutes, 0);
    nc_config_free(&config);

    {
        NcStringList names;
        size_t index;
        nc_string_list_init(&names);
        for (index = 0; index < 80; index++) {
            char item[32];
            snprintf(item, sizeof(item), "App%u.EXE", (unsigned)index);
            nc_string_list_push(&names, item);
        }
        nc_string_list_push(&names, "app0.exe");
        nc_config_sanitize_process_names(&names);
        CHECK_INT(names.count, NC_MAX_PROCESS_COUNT);
        if (names.count > 0) {
            CHECK_STR(names.items[0], "app0.exe");
        }
        {
            size_t duplicates = 0;
            for (index = 0; index < names.count; index++) {
                if (nc_ieq_ascii(names.items[index], "app0.exe")) {
                    duplicates++;
                }
            }
            CHECK_INT(duplicates, 1);
        }
        nc_string_list_free(&names);
    }

    /* Validation messages and ranges. */
    {
        NcProfile profile;
        memset(&profile, 0, sizeof(profile));
        profile.name = nc_strdup("");
        CHECK(!nc_profile_validate(&profile, &error));
        CHECK_STR(error.msg, "配置名称不能为空。");
        free(profile.name);
        profile.name = nc_strdup("ok");
        profile.uplink.delay_ms = -1;
        CHECK(!nc_profile_validate(&profile, &error));
        CHECK_STR(error.msg, "上行延时必须在 0 到 60000 ms 之间。");
        nc_config_free_profile(&profile);
    }
}