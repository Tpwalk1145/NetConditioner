#include "app/licenses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/str.h"
#include "core/time.h"
#include "core/win.h"
#include "nc_version.h"

int nc_licenses_run(void) {
    WStr base;
    WStr directory;
    WStr licenses;
    WStr path;
    wchar_t name[128];
    HRSRC resource;
    HGLOBAL data;
    const void *bytes;
    DWORD size;
    char id[33];
    wchar_t wide_id[64];
    NcErr error;
    int code = 1;
    wstr_init(&base);
    wstr_init(&directory);
    wstr_init(&licenses);
    wstr_init(&path);
    if (!nc_temp_dir_w(&base) ||
        !nc_path_join_w(&directory, wstr_cstr(&base), L"NetConditioner") ||
        !nc_path_join_w(&licenses, wstr_cstr(&directory), L"licenses")) {
        goto done;
    }
    nc_make_dirs(wstr_cstr(&licenses));
    nc_new_id(id);
    nc_write_utf8(wide_id, sizeof(wide_id) / sizeof(wide_id[0]), id);
    swprintf(name, sizeof(name) / sizeof(name[0]), L"WinDivert-LICENSE-%ls.txt", wide_id);
    if (!nc_path_join_w(&path, wstr_cstr(&licenses), name)) {
        goto done;
    }
    resource = FindResourceW(NULL, L"WINDIVERT_LICENSE", RT_RCDATA);
    if (!resource) {
        nc_message_box(NULL, "程序缺少 WinDivert 许可资源。", "NetConditioner 许可",
                       MB_OK | MB_ICONINFORMATION);
        goto done;
    }
    size = SizeofResource(NULL, resource);
    data = LoadResource(NULL, resource);
    bytes = data ? LockResource(data) : NULL;
    if (!bytes || !nc_write_file(wstr_cstr(&path), bytes, size, &error)) {
        nc_message_box(NULL, "写入许可文本失败。", "NetConditioner 许可", MB_OK | MB_ICONINFORMATION);
        goto done;
    }
    {
        Str notice;
        str_init(&notice);
        str_addf(&notice,
                 "本程序动态加载随包分发的 WinDivert %s（LGPLv3 / GPLv2 双许可）。\r\n\r\n"
                 "完整许可文本已保存到：\r\n%ls\r\n\r\nWinDivert 源码：https://github.com/basil00/WinDivert",
                 NC_WINDIVERT_VERSION, wstr_cstr(&path));
        nc_message_box(NULL, str_cstr(&notice), "NetConditioner 许可", MB_OK | MB_ICONINFORMATION);
        str_free(&notice);
    }
    code = 0;
done:
    wstr_free(&path);
    wstr_free(&licenses);
    wstr_free(&directory);
    wstr_free(&base);
    return code;
}
