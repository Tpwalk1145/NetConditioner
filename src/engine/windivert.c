#include "windivert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_version.h"

typedef INT_PTR(WINAPI *WinDivertOpenFn)(const char *, int, INT16, UINT64);
typedef BOOL(WINAPI *WinDivertRecvFn)(INT_PTR, void *, UINT, UINT *, void *);
typedef BOOL(WINAPI *WinDivertSendFn)(INT_PTR, const void *, UINT, UINT *, const void *);
typedef BOOL(WINAPI *WinDivertCloseFn)(INT_PTR);
typedef BOOL(WINAPI *WinDivertShutdownFn)(INT_PTR, int);
typedef BOOL(WINAPI *WinDivertSetParamFn)(INT_PTR, int, UINT64);
typedef BOOL(WINAPI *WinDivertHelperParsePacketFn)(
    const void *, UINT, void **, void **, uint8_t *, void **, void **, void **, void **,
    void **, UINT *, void **, UINT *);

typedef struct {
    WinDivertOpenFn open;
    WinDivertRecvFn recv;
    WinDivertSendFn send;
    WinDivertCloseFn close;
    WinDivertShutdownFn shutdown;
    WinDivertSetParamFn set_param;
    WinDivertHelperParsePacketFn parse_packet;
} WinDivertApi;

static SRWLOCK g_api_lock = SRWLOCK_INIT;
static WinDivertApi g_api;
static bool g_api_loaded = false;

const char *nc_windivert_payload_version(void) {
    return NC_WINDIVERT_VERSION;
}

static bool delete_tree(const wchar_t *path) {
    wchar_t pattern[32768];
    WIN32_FIND_DATAW data;
    HANDLE find;
    bool ok = true;
    if (!nc_path_join(pattern, sizeof(pattern) / sizeof(pattern[0]), path, L"*")) {
        return false;
    }
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryW(path) != 0;
    }
    do {
        wchar_t child[32768];
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        if (!nc_path_join(child, sizeof(child) / sizeof(child[0]), path, data.cFileName)) {
            ok = false;
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!delete_tree(child)) {
                ok = false;
            }
        } else if (!DeleteFileW(child)) {
            ok = false;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (!RemoveDirectoryW(path)) {
        ok = false;
    }
    return ok;
}

static void cleanup_old_versions(const wchar_t *dll_path) {
    wchar_t current_dir[32768];
    wchar_t root[32768];
    wchar_t pattern[32768];
    WIN32_FIND_DATAW data;
    HANDLE find;
    current_dir[0] = 0;
    root[0] = 0;
    if (!nc_path_parent(dll_path, current_dir, sizeof(current_dir) / sizeof(current_dir[0]))) {
        return;
    }
    if (!nc_path_parent(current_dir, root, sizeof(root) / sizeof(root[0]))) {
        return;
    }
    if (!nc_path_join(pattern, sizeof(pattern) / sizeof(pattern[0]), root, L"*")) {
        return;
    }
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        wchar_t child[32768];
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        if (!nc_path_join(child, sizeof(child) / sizeof(child[0]), root, data.cFileName)) {
            continue;
        }
        if (_wcsicmp(child, current_dir) == 0) {
            continue;
        }
        delete_tree(child);
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

static bool write_resource_if_needed(const wchar_t *directory, const wchar_t *name,
                                     const void *bytes, size_t size, NcErr *err) {
    wchar_t path[32768];
    void *existing = NULL;
    size_t existing_size = 0;
    bool equals = false;
    NcErr read_error;
    if (!nc_path_join(path, sizeof(path) / sizeof(path[0]), directory, name)) {
        return nc_fail(err, "运行时文件路径过长。");
    }
    if (nc_read_file(path, size, &existing, &existing_size, &read_error)) {
        if (existing_size == size && (size == 0 || memcmp(existing, bytes, size) == 0)) {
            equals = true;
        }
    }
    free(existing);
    if (equals) {
        return true;
    }
    {
        wchar_t temp[32768];
        HANDLE handle = INVALID_HANDLE_VALUE;
        NcErr write_error;
        if (!nc_create_temp_file(directory, L"WindDivert", bytes, size, temp,
                                 sizeof(temp) / sizeof(temp[0]), &handle, &write_error)) {
            return nc_fail(err, "写入 %ls 失败：%s", path, write_error.msg);
        }
        CloseHandle(handle);
        if (!nc_replace_file(temp, path, &write_error)) {
            nc_remove_file(temp);
            return nc_fail(err, "替换 %ls 失败：%s", path, write_error.msg);
        }
    }
    return true;
}

static bool ensure_payload_dir(const wchar_t *directory, wchar_t *dll_path, size_t cap, NcErr *err) {
    HRSRC dll_resource;
    HRSRC sys_resource;
    HGLOBAL dll_data;
    HGLOBAL sys_data;
    const void *dll_bytes;
    const void *sys_bytes;
    DWORD dll_size;
    DWORD sys_size;
    if (!nc_make_dirs(directory)) {
        return nc_fail(err, "创建目录失败（%ls）：Win32 错误 %lu", directory,
                       (unsigned long)GetLastError());
    }
    dll_resource = FindResourceW(NULL, L"WINDIVERT_DLL", RT_RCDATA);
    sys_resource = FindResourceW(NULL, L"WINDIVERT_SYS", RT_RCDATA);
    if (!dll_resource || !sys_resource) {
        return nc_fail(err, "程序缺少 WinDivert 运行时资源。");
    }
    dll_size = SizeofResource(NULL, dll_resource);
    sys_size = SizeofResource(NULL, sys_resource);
    dll_data = LoadResource(NULL, dll_resource);
    sys_data = LoadResource(NULL, sys_resource);
    dll_bytes = dll_data ? LockResource(dll_data) : NULL;
    sys_bytes = sys_data ? LockResource(sys_data) : NULL;
    if (!dll_bytes || !sys_bytes) {
        return nc_fail(err, "读取 WinDivert 运行时资源失败。");
    }
    if (!write_resource_if_needed(directory, L"WinDivert.dll", dll_bytes, dll_size, err)) {
        return false;
    }
    if (!write_resource_if_needed(directory, L"WinDivert64.sys", sys_bytes, sys_size, err)) {
        return false;
    }
    if (!nc_path_join(dll_path, cap, directory, L"WinDivert.dll")) {
        return nc_fail(err, "运行时文件路径过长。");
    }
    return true;
}

static bool resolve_dll_path(wchar_t *out, size_t cap, NcErr *err) {
    static const wchar_t *variables[] = {L"ProgramFiles", L"ProgramData", L"LOCALAPPDATA"};
    wchar_t runtime_root[32768];
    wchar_t directory[32768];
    wchar_t candidate[32768];
    size_t index;
    NcErr last_error;
    nc_err_set(&last_error, "没有可用的运行时目录。");
    for (index = 0; index < sizeof(variables) / sizeof(variables[0]); index++) {
        wchar_t base[32768];
        if (!nc_env_absolute_dir(variables[index], base, sizeof(base) / sizeof(base[0]))) {
            continue;
        }
        if (!nc_path_join(runtime_root, sizeof(runtime_root) / sizeof(runtime_root[0]), base,
                          L"NetConditioner")) {
            continue;
        }
        if (!nc_path_join(directory, sizeof(directory) / sizeof(directory[0]), runtime_root,
                          L"runtime")) {
            continue;
        }
        if (!nc_path_join(candidate, sizeof(candidate) / sizeof(candidate[0]), directory,
                          NC_WINDIVERT_VERSION_WSTR)) {
            continue;
        }
        if (ensure_payload_dir(candidate, out, cap, &last_error)) {
            cleanup_old_versions(out);
            return true;
        }
    }
    return nc_fail(err, "无法准备 WinDivert 运行时：%s", last_error.msg);
}

static bool load_api(WinDivertApi *api, NcErr *err) {
    wchar_t dll_path[32768];
    HMODULE module;
    if (!resolve_dll_path(dll_path, sizeof(dll_path) / sizeof(dll_path[0]), err)) {
        return false;
    }
    module = LoadLibraryExW(dll_path, NULL, 0x00000008); /* LOAD_WITH_ALTERED_SEARCH_PATH */
    if (!module) {
        return nc_fail(err, "无法加载 WinDivert.dll（%ls）：Win32 错误 %lu", dll_path,
                       (unsigned long)GetLastError());
    }
    memset(api, 0, sizeof(*api));
    api->open = (WinDivertOpenFn)GetProcAddress(module, "WinDivertOpen");
    api->recv = (WinDivertRecvFn)GetProcAddress(module, "WinDivertRecv");
    api->send = (WinDivertSendFn)GetProcAddress(module, "WinDivertSend");
    api->close = (WinDivertCloseFn)GetProcAddress(module, "WinDivertClose");
    api->shutdown = (WinDivertShutdownFn)GetProcAddress(module, "WinDivertShutdown");
    api->set_param = (WinDivertSetParamFn)GetProcAddress(module, "WinDivertSetParam");
    api->parse_packet = (WinDivertHelperParsePacketFn)GetProcAddress(module, "WinDivertHelperParsePacket");
    if (!api->open || !api->recv || !api->send || !api->close || !api->shutdown ||
        !api->set_param || !api->parse_packet) {
        return nc_fail(err, "WinDivert.dll 缺少导出函数。请使用随程序附带的 %ls 官方文件。",
                       NC_WINDIVERT_VERSION_WSTR);
    }
    return true;
}

static bool api_pointer(WinDivertApi **out, NcErr *err) {
    if (!out) {
        return nc_fail(err, "内部错误。");
    }
    AcquireSRWLockShared(&g_api_lock);
    if (g_api_loaded) {
        *out = &g_api;
        ReleaseSRWLockShared(&g_api_lock);
        return true;
    }
    ReleaseSRWLockShared(&g_api_lock);
    AcquireSRWLockExclusive(&g_api_lock);
    if (!g_api_loaded) {
        NcErr load_error;
        if (!load_api(&g_api, &load_error)) {
            ReleaseSRWLockExclusive(&g_api_lock);
            if (err) {
                *err = load_error;
            }
            return false;
        }
        g_api_loaded = true;
    }
    *out = &g_api;
    ReleaseSRWLockExclusive(&g_api_lock);
    return true;
}

static const char *describe_open_error(int code) {
    switch (code) {
    case 2:
        return "找不到 WinDivert64.sys。运行时文件可能不完整，请删除 %ProgramFiles%\\NetConditioner\\runtime 后重试。";
    case 5:
        return "拒绝访问。WinDivert 需要管理员权限，请以管理员身份运行。";
    case 577:
        return "WinDivert 驱动签名无效或被系统拦截。请使用随程序附带的官方 2.2.2 文件，并检查杀毒软件。";
    case 1275:
        return "WinDivert 驱动被系统策略阻止加载（错误 1275）。请检查驱动签名/安全软件。";
    case 1058:
        return "WinDivert 驱动服务无法启动。请检查系统服务与安全软件。";
    default:
        return NULL;
    }
}

bool nc_windivert_open(const char *filter, INT_PTR *handle_out, NcErr *err) {
    WinDivertApi *api;
    INT_PTR handle;
    if (!handle_out) {
        return nc_fail(err, "内部错误。");
    }
    *handle_out = 0;
    if (!api_pointer(&api, err)) {
        return false;
    }
    handle = api->open(filter ? filter : "false", NC_WINDIVERT_LAYER_NETWORK, 0, 0);
    if (handle == 0 || handle == -1) {
        int code = (int)GetLastError();
        const char *known = describe_open_error(code);
        if (known) {
            return nc_fail(err, "%s", known);
        }
        return nc_fail(err,
                       "WinDivert 启动失败（Win32 错误 %d）。常见原因：未以管理员运行、驱动文件缺失、"
                       "杀毒软件拦截或已有不兼容的 WinDivert 驱动。",
                       code);
    }
    *handle_out = handle;
    return true;
}

void nc_windivert_set_params(INT_PTR handle) {
    WinDivertApi *api;
    NcErr error;
    if (!api_pointer(&api, &error)) {
        return;
    }
    api->set_param(handle, 0, 16384);
    api->set_param(handle, 1, 2000);
    api->set_param(handle, 2, 32u * 1024u * 1024u);
}

bool nc_windivert_recv(INT_PTR handle, void *packet, UINT *length, void *address, int *error_code) {
    WinDivertApi *api;
    NcErr error;
    UINT received = 0;
    if (!api_pointer(&api, &error)) {
        if (error_code) {
            *error_code = 0;
        }
        return false;
    }
    if (!api->recv(handle, packet, NC_MAX_PACKET_SIZE, &received, address)) {
        if (error_code) {
            *error_code = (int)GetLastError();
        }
        return false;
    }
    if (length) {
        *length = received;
    }
    return true;
}

bool nc_windivert_send(INT_PTR handle, const void *packet, UINT length, const void *address,
                       int *error_code) {
    WinDivertApi *api;
    NcErr error;
    UINT sent = 0;
    if (!api_pointer(&api, &error)) {
        if (error_code) {
            *error_code = 0;
        }
        return false;
    }
    if (!api->send(handle, packet, length, &sent, address)) {
        if (error_code) {
            *error_code = (int)GetLastError();
        }
        return false;
    }
    return true;
}

void nc_windivert_close(INT_PTR handle) {
    WinDivertApi *api;
    NcErr error;
    if (handle && api_pointer(&api, &error)) {
        api->close(handle);
    }
}

bool nc_windivert_shutdown(INT_PTR handle, int how) {
    WinDivertApi *api;
    NcErr error;
    if (!handle || !api_pointer(&api, &error)) {
        return false;
    }
    return api->shutdown(handle, how) != 0;
}

bool nc_windivert_parse_protocol(const void *packet, UINT length, uint8_t *protocol) {
    WinDivertApi *api;
    NcErr error;
    void *ip_header = NULL;
    void *ipv6_header = NULL;
    void *icmp_header = NULL;
    void *icmpv6_header = NULL;
    void *tcp_header = NULL;
    void *udp_header = NULL;
    void *data = NULL;
    UINT data_length = 0;
    void *next = NULL;
    UINT next_length = 0;
    uint8_t value = 0;
    if (!protocol || !api_pointer(&api, &error)) {
        return false;
    }
    if (!api->parse_packet(packet, length, &ip_header, &ipv6_header, &value, &icmp_header,
                           &icmpv6_header, &tcp_header, &udp_header, &data, &data_length,
                           &next, &next_length)) {
        return false;
    }
    *protocol = value;
    return true;
}
