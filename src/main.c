#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "licenses.h"
#include "selftest.h"
#include "update/checkupdate.h"
#include "update/selfupdate.h"
#include "win.h"

static bool has_flag(int argc, wchar_t **argv, const wchar_t *flag) {
    int index;
    for (index = 0; index < argc; index++) {
        if (_wcsicmp(argv[index], flag) == 0) {
            return true;
        }
    }
    return false;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
    wchar_t message[512];
    (void)info;
    swprintf(message, sizeof(message) / sizeof(message[0]),
             L"发生未知错误。\r\n\r\n程序将退出，网络设置会随进程结束恢复。");
    MessageBoxW(NULL, message, L"NetConditioner", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    int argc = 0;
    wchar_t **argv;
    HANDLE mutex;
    int code = 0;
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 1;
    }

    if (nc_selfupdate_is_helper_mode(argc, argv)) {
        code = nc_selfupdate_run_helper(argc, argv);
        LocalFree(argv);
        return code;
    }
    if (has_flag(argc, argv, L"--licenses")) {
        code = nc_licenses_run();
        LocalFree(argv);
        return code;
    }
    if (has_flag(argc, argv, L"--self-test")) {
        code = nc_selftest_run();
        LocalFree(argv);
        return code;
    }
    if (has_flag(argc, argv, L"--check-update")) {
        code = nc_checkupdate_run();
        LocalFree(argv);
        return code;
    }
    LocalFree(argv);

    nc_set_dpi_awareness();
    nc_init_common_controls();

    mutex = nc_create_single_instance_mutex();
    if (!mutex) {
        nc_message_box(NULL, "弱网模拟器已经在运行。", "NetConditioner", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (!nc_is_elevated()) {
        nc_message_box(NULL, "WinDivert 需要管理员权限。请右键选择“以管理员身份运行”。",
                       "NetConditioner", MB_OK | MB_ICONWARNING);
        CloseHandle(mutex);
        return 0;
    }
    SetUnhandledExceptionFilter(crash_filter);
    code = nc_app_run();
    if (mutex != INVALID_HANDLE_VALUE) {
        CloseHandle(mutex);
    }
    return code;
}