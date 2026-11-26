#include "checkupdate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store.h"
#include "update.h"
#include "version.h"

int nc_checkupdate_run(void) {
    wchar_t base[32768];
    wchar_t log_path[32768];
    char id[33];
    wchar_t wide_id[64];
    wchar_t log_name[256];
    NcStore store;
    NcConfig config;
    char warning[2048];
    Str lines;
    char version_text[64];
    int exit_code = 1;
    nc_store_init(&store);
    if (!nc_store_load(&store, &config, warning, sizeof(warning))) {
        return 1;
    }
    if (!nc_env_absolute_dir(L"TEMP", base, sizeof(base) / sizeof(base[0]))) {
        DWORD length = GetTempPathW(sizeof(base) / sizeof(base[0]), base);
        if (length == 0) {
            nc_config_free(&config);
            return 1;
        }
    }
    nc_new_id(id);
    nc_write_utf8(wide_id, sizeof(wide_id) / sizeof(wide_id[0]), id);
    swprintf(log_name, sizeof(log_name) / sizeof(log_name[0]),
             L"NetConditioner-update-check-%ls.log", wide_id);
    nc_path_join(log_path, sizeof(log_path) / sizeof(log_path[0]), base, log_name);
    str_init(&lines);
    nc_version_display(version_text, sizeof(version_text));
    {
        char stamp[32];
        nc_format_local_hms(stamp, sizeof(stamp));
        str_addf(&lines, "[%s] 当前版本：%s\r\n", stamp, version_text);
        nc_format_local_hms(stamp, sizeof(stamp));
        str_addf(&lines, "[%s] 更新地址：%s\r\n", stamp,
                 config.settings.update_feed_url && config.settings.update_feed_url[0]
                     ? config.settings.update_feed_url
                     : "(未配置)");
    }
    if (!config.settings.update_feed_url || !config.settings.update_feed_url[0]) {
        char stamp[32];
        nc_format_local_hms(stamp, sizeof(stamp));
        str_addf(&lines, "[%s] 结果：未配置更新地址，云更新已关闭。\r\n", stamp);
    } else {
        NcVersion current = nc_version_current();
        NcUpdateCheckResult result = nc_update_check(config.settings.update_feed_url, &current);
        char stamp[32];
        nc_format_local_hms(stamp, sizeof(stamp));
        switch (result.kind) {
        case NC_UPDATE_CHECK_AVAILABLE:
            str_addf(&lines, "[%s] 结果：发现新版本 %s。\r\n", stamp,
                     result.manifest.version ? result.manifest.version : "");
            nc_format_local_hms(stamp, sizeof(stamp));
            str_addf(&lines, "[%s] 更新包：%s\r\n", stamp,
                     result.manifest.installer.url ? result.manifest.installer.url : "");
            nc_format_local_hms(stamp, sizeof(stamp));
            str_addf(&lines, "[%s] SHA256：%s\r\n", stamp,
                     result.manifest.installer.sha256 ? result.manifest.installer.sha256 : "");
            exit_code = 0;
            break;
        case NC_UPDATE_CHECK_UP_TO_DATE:
            str_addf(&lines, "[%s] 结果：已是最新版本。\r\n", stamp);
            exit_code = 0;
            break;
        case NC_UPDATE_CHECK_NOT_CONFIGURED:
            str_addf(&lines, "[%s] 结果：未配置更新地址 - %s\r\n", stamp,
                     result.error ? result.error : "");
            break;
        case NC_UPDATE_CHECK_FAILED:
            str_addf(&lines, "[%s] 结果：检查失败 - %s\r\n", stamp,
                     result.error ? result.error : "");
            break;
        default:
            break;
        }
        nc_update_check_result_free(&result);
    }
    {
        NcErr error;
        void *data = str_take(&lines);
        if (data) {
            nc_write_file(log_path, data, strlen((char *)data), &error);
            free(data);
        }
    }
    nc_config_free(&config);
    return exit_code;
}
