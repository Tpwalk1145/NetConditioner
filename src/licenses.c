#include "licenses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_version.h"
#include "win.h"

int nc_licenses_run(void) {
    wchar_t base[32768];
    wchar_t directory[32768];
    wchar_t licenses[32768];
    wchar_t name[128];
    wchar_t path[32768];
    HRSRC resource;
    HGLOBAL data;
    const void *bytes;
    DWORD size;
    char id[33];
    wchar_t wide_id[64];
    NcErr error;
    if (!nc_env_absolute_dir(L"TEMP", base, sizeof(base) / sizeof(base[0]))) {
        DWORD length = GetTempPathW(sizeof(base) / sizeof(base[0]), base);
        if (length == 0) {
            return 1;
        }
    }
    if (!nc_path_join(directory, sizeof(directory) / sizeof(directory[0]), base,
                      L"NetConditioner")) {
        return 1;
    }
    if (!nc_path_join(licenses, sizeof(licenses) / sizeof(licenses[0]), directory, L"licenses")) {
        return 1;
    }
    nc_make_dirs(licenses);
    nc_new_id(id);
    nc_write_utf8(wide_id, sizeof(wide_id) / sizeof(wide_id[0]), id);
    swprintf(name, sizeof(name) / sizeof(name[0]), L"WinDivert-LICENSE-%ls.txt", wide_id);
    if (!nc_path_join(path, sizeof(path) / sizeof(path[0]), licenses, name)) {
        return 1;
    }
    resource = FindResourceW(NULL, L"WINDIVERT_LICENSE", RT_RCDATA);
    if (!resource) {
        nc_message_box(NULL, "程序缺少 WinDivert 许可资源。", "NetConditioner 许可",
                       MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    size = SizeofResource(NULL, resource);
    data = LoadResource(NULL, resource);
    bytes = data ? LockResource(data) : NULL;
    if (!bytes || !nc_write_file(path, bytes, size, &error)) {
        nc_message_box(NULL, "写入许可文本失败。", "NetConditioner 许可", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    {
        Str notice;
        str_init(&notice);
        str_addf(&notice,
                 "本程序动态加载随包分发的 WinDivert %s（LGPLv3 / GPLv2 双许可）。\r\n\r\n"
                 "完整许可文本已保存到：\r\n%ls\r\n\r\nWinDivert 源码：https://github.com/basil00/WinDivert",
                 NC_WINDIVERT_VERSION, path);
        nc_message_box(NULL, str_cstr(&notice), "NetConditioner 许可", MB_OK | MB_ICONINFORMATION);
        str_free(&notice);
    }
    return 0;
}