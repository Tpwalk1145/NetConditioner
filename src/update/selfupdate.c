#include "selfupdate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neturl.h"
#include "update.h"

typedef struct {
    wchar_t target[32768];
    wchar_t new_file[32768];
    wchar_t ready[256];
    DWORD parent_pid;
    bool restart;
    char sha256[65];
    uint64_t size;
} ApplyArgs;

static const wchar_t *arg_value(int argc, wchar_t **argv, const wchar_t *name) {
    int index;
    for (index = 0; index + 1 < argc; index++) {
        if (_wcsicmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }
    return NULL;
}

static bool has_arg(int argc, wchar_t **argv, const wchar_t *name) {
    int index;
    for (index = 0; index < argc; index++) {
        if (_wcsicmp(argv[index], name) == 0) {
            return true;
        }
    }
    return false;
}

bool nc_selfupdate_is_helper_mode(int argc, wchar_t **argv) {
    return has_arg(argc, argv, L"--apply-update");
}

static bool parse_args(int argc, wchar_t **argv, ApplyArgs *args) {
    const wchar_t *value;
    char *sha_utf8;
    char normalized[65];
    if (!args) {
        return false;
    }
    memset(args, 0, sizeof(*args));
    value = arg_value(argc, argv, L"--target");
    if (!value || !nc_copy_wide(args->target, sizeof(args->target) / sizeof(args->target[0]), value)) {
        return false;
    }
    value = arg_value(argc, argv, L"--new");
    if (!value || !nc_copy_wide(args->new_file, sizeof(args->new_file) / sizeof(args->new_file[0]), value)) {
        return false;
    }
    value = arg_value(argc, argv, L"--pid");
    if (!value) {
        return false;
    }
    args->parent_pid = (DWORD)wcstoul(value, NULL, 10);
    value = arg_value(argc, argv, L"--sha256");
    if (!value) {
        return false;
    }
    sha_utf8 = nc_utf8_dup(value);
    if (!sha_utf8) {
        return false;
    }
    if (!nc_neturl_normalize_sha256(sha_utf8, normalized) || strcmp(normalized, sha_utf8) != 0) {
        free(sha_utf8);
        return false;
    }
    snprintf(args->sha256, sizeof(args->sha256), "%s", normalized);
    free(sha_utf8);
    value = arg_value(argc, argv, L"--size");
    if (!value) {
        return false;
    }
    args->size = _wcstoui64(value, NULL, 10);
    if (args->size == 0) {
        return false;
    }
    value = arg_value(argc, argv, L"--ready");
    if (!value || !nc_copy_wide(args->ready, sizeof(args->ready) / sizeof(args->ready[0]), value)) {
        return false;
    }
    args->restart = !has_arg(argc, argv, L"--no-restart");
    return true;
}

static void signal_ready(const wchar_t *name) {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (event) {
        SetEvent(event);
        CloseHandle(event);
    }
}

static bool start_process(const wchar_t *path) {
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

static int fail_and_restart(const ApplyArgs *args, const char *message) {
    nc_update_log(message);
    if (args->restart) {
        if (start_process(args->target)) {
            nc_update_log("已重新启动原版本。");
            return 0;
        }
        nc_update_log("重新启动原版本失败。");
        return 3;
    }
    return 3;
}

static bool wait_parent(DWORD pid, DWORD timeout_ms) {
    HANDLE process;
    DWORD result;
    if (pid == 0) {
        return false;
    }
    process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return false;
    }
    result = WaitForSingleObject(process, timeout_ms);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}

static bool write_named_file(const wchar_t *directory, const wchar_t *name, const void *data,
                             size_t size, wchar_t *out, size_t cap, NcErr *err) {
    wchar_t path[32768];
    if (!nc_path_join(path, sizeof(path) / sizeof(path[0]), directory, name)) {
        return nc_fail(err, "更新文件路径过长。");
    }
    if (!nc_write_file(path, data, size, err)) {
        return false;
    }
    return nc_copy_wide(out, cap, path);
}

int nc_selfupdate_run_helper(int argc, wchar_t **argv) {
    ApplyArgs args;
    wchar_t directory[32768];
    NcErr error;
    uint64_t source_size = 0;
    char source_hash[65];
    void *source_bytes = NULL;
    size_t source_length = 0;
    void *target_bytes = NULL;
    size_t target_length = 0;
    char target_hash[65];
    wchar_t stage_path[32768] = {0};
    wchar_t backup_path[32768] = {0};
    char message[1200];
    if (!parse_args(argc, argv, &args)) {
        return 2;
    }
    signal_ready(args.ready);
    if (!nc_update_ensure_directory(directory, sizeof(directory) / sizeof(directory[0]), &error)) {
        snprintf(message, sizeof(message), "自更新失败：%s", error.msg);
        return fail_and_restart(&args, message);
    }
    if (!wait_parent(args.parent_pid, 30000)) {
        return fail_and_restart(&args, "自更新失败：父进程未在期限内正常退出，未替换文件。");
    }
    if (!nc_sha256_file(args.new_file, &source_size, source_hash, &error)) {
        snprintf(message, sizeof(message), "自更新失败：无法校验更新包（%s）。", error.msg);
        return fail_and_restart(&args, message);
    }
    if (source_size != args.size || !nc_ieq_ascii(source_hash, args.sha256)) {
        return fail_and_restart(&args, "自更新失败：更新包 SHA256 或大小与父进程不一致。");
    }
    if (!nc_read_file(args.new_file, 400ull * 1024ull * 1024ull, &source_bytes, &source_length,
                      &error)) {
        snprintf(message, sizeof(message), "自更新失败：无法读取更新包（%s）。", error.msg);
        return fail_and_restart(&args, message);
    }
    {
        wchar_t target_dir[32768];
        target_dir[0] = 0;
        nc_path_parent(args.target, target_dir, sizeof(target_dir) / sizeof(target_dir[0]));
        if (target_dir[0] == 0) {
            nc_copy_wide(target_dir, sizeof(target_dir) / sizeof(target_dir[0]), L".");
        }
        if (!nc_create_temp_file(target_dir, L"nc-self-stage", source_bytes, source_length, stage_path,
                                 sizeof(stage_path) / sizeof(stage_path[0]), NULL, &error)) {
            free(source_bytes);
            snprintf(message, sizeof(message), "自更新失败：无法创建暂存文件（%s）。", error.msg);
            return fail_and_restart(&args, message);
        }
        {
            uint64_t staged_size = 0;
            char staged_hash[65];
            if (!nc_sha256_file(stage_path, &staged_size, staged_hash, &error) ||
                staged_size != args.size || !nc_ieq_ascii(staged_hash, args.sha256)) {
                free(source_bytes);
                nc_remove_file(stage_path);
                return fail_and_restart(&args, "自更新失败：暂存文件复核失败。");
            }
        }
        if (!nc_read_file(args.target, 400ull * 1024ull * 1024ull, &target_bytes, &target_length,
                          &error)) {
            free(source_bytes);
            nc_remove_file(stage_path);
            snprintf(message, sizeof(message), "自更新失败：无法读取原程序（%s）。", error.msg);
            return fail_and_restart(&args, message);
        }
        nc_sha256_hex(target_bytes, target_length, target_hash);
        if (!write_named_file(target_dir, L"nc-self-backup.tmp", target_bytes, target_length,
                              backup_path, sizeof(backup_path) / sizeof(backup_path[0]), &error)) {
            free(source_bytes);
            free(target_bytes);
            nc_remove_file(stage_path);
            snprintf(message, sizeof(message), "自更新失败：无法备份原程序（%s）。", error.msg);
            return fail_and_restart(&args, message);
        }
        free(target_bytes);
        free(source_bytes);
        {
            uint64_t verify_size = 0;
            char verify_hash[65];
            if (!nc_sha256_file(args.target, &verify_size, verify_hash, &error) ||
                verify_size != target_length || !nc_ieq_ascii(verify_hash, target_hash)) {
                nc_remove_file(stage_path);
                nc_remove_file(backup_path);
                return fail_and_restart(&args, "自更新失败：原程序在备份期间发生变化。");
            }
        }
        if (!ReplaceFileW(args.target, stage_path, backup_path, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL,
                          NULL)) {
            DWORD code = GetLastError();
            nc_remove_file(stage_path);
            if (backup_path[0]) {
                nc_replace_file(backup_path, args.target, &error);
            }
            snprintf(message, sizeof(message), "自更新失败：替换文件失败（Win32 错误 %lu）。",
                     (unsigned long)code);
            return fail_and_restart(&args, message);
        }
        nc_selfupdate_remove_zone_identifier(args.target);
        nc_update_log("自更新完成。");
        if (args.restart) {
            if (!start_process(args.target)) {
                if (backup_path[0]) {
                    nc_replace_file(backup_path, args.target, &error);
                }
                nc_update_log("新版本启动失败，已回滚。");
                return fail_and_restart(&args, "自更新已替换，但新版本启动失败。");
            }
            nc_update_log("已重新启动新版本。");
        }
        if (backup_path[0]) {
            nc_remove_file(backup_path);
        }
        return 0;
    }
}

static void append_quoted(Str *text, const wchar_t *value) {
    char *utf8 = nc_utf8_dup(value);
    if (!utf8) {
        return;
    }
    str_add(text, "\"");
    str_add(text, utf8);
    str_add(text, "\"");
    free(utf8);
}

bool nc_selfupdate_spawn_helper(const wchar_t *new_file, NcErr *err) {
    wchar_t current[32768];
    wchar_t directory[32768];
    wchar_t helper_name[256];
    wchar_t helper_path[32768];
    char id[33];
    void *current_bytes = NULL;
    size_t current_length = 0;
    NcErr local;
    uint64_t payload_size = 0;
    char payload_hash[65];
    wchar_t ready_name[256];
    HANDLE ready_event = NULL;
    Str command;
    char *command_utf8;
    wchar_t *command_wide = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    bool ok = false;
    if (!nc_get_module_path(current, sizeof(current) / sizeof(current[0]))) {
        return nc_fail(err, "读取当前程序路径失败。");
    }
    if (!nc_update_ensure_directory(directory, sizeof(directory) / sizeof(directory[0]), &local)) {
        return nc_fail(err, "%s", local.msg);
    }
    if (!nc_read_file(current, 400ull * 1024ull * 1024ull, &current_bytes, &current_length, &local)) {
        return nc_fail(err, "打开更新程序失败：%s", local.msg);
    }
    nc_new_id(id);
    {
        wchar_t wide_id[64];
        nc_write_utf8(wide_id, sizeof(wide_id) / sizeof(wide_id[0]), id);
        swprintf(helper_name, sizeof(helper_name) / sizeof(helper_name[0]),
                 L"NetConditioner-updater-%ls.exe", wide_id);
        swprintf(ready_name, sizeof(ready_name) / sizeof(ready_name[0]),
                 L"Local\\NetConditioner-update-ready-%ls", wide_id);
    }
    if (!nc_path_join(helper_path, sizeof(helper_path) / sizeof(helper_path[0]), directory,
                      helper_name)) {
        free(current_bytes);
        return nc_fail(err, "更新程序路径过长。");
    }
    {
        HANDLE handle = CreateFileW(helper_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        if (handle == INVALID_HANDLE_VALUE ||
            !WriteFile(handle, current_bytes, (DWORD)current_length, &written, NULL)) {
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
            free(current_bytes);
            nc_remove_file(helper_path);
            return nc_fail(err, "准备更新程序失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        }
        CloseHandle(handle);
        free(current_bytes);
    }
    if (!nc_sha256_file(new_file, &payload_size, payload_hash, &local) || payload_size == 0) {
        nc_remove_file(helper_path);
        return nc_fail(err, "校验更新包失败：%s", local.msg);
    }
    ready_event = CreateEventW(NULL, TRUE, FALSE, ready_name);
    if (!ready_event) {
        nc_remove_file(helper_path);
        return nc_fail(err, "创建更新握手事件失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
    }
    str_init(&command);
    append_quoted(&command, helper_path);
    str_add(&command, " --apply-update");
    str_add(&command, " --target ");
    append_quoted(&command, current);
    str_add(&command, " --new ");
    append_quoted(&command, new_file);
    str_addf(&command, " --pid %lu --sha256 %s --size %llu --ready ", (unsigned long)GetCurrentProcessId(),
             payload_hash, (unsigned long long)payload_size);
    append_quoted(&command, ready_name);
    command_utf8 = str_take(&command);
    command_wide = command_utf8 ? nc_wide_dup(command_utf8) : NULL;
    free(command_utf8);
    if (!command_wide) {
        CloseHandle(ready_event);
        nc_remove_file(helper_path);
        return nc_fail(err, "内存不足。");
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(helper_path, command_wide, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process)) {
        DWORD code = GetLastError();
        free(command_wide);
        CloseHandle(ready_event);
        nc_remove_file(helper_path);
        return nc_fail(err, "启动更新程序失败（Win32 错误 %lu）。", (unsigned long)code);
    }
    free(command_wide);
    {
        DWORD wait = WaitForSingleObject(ready_event, 10000);
        CloseHandle(ready_event);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            nc_remove_file(helper_path);
            return nc_fail(err, "更新程序握手超时或失败。");
        }
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ok = true;
    return ok;
}

void nc_selfupdate_remove_zone_identifier(const wchar_t *path) {
    wchar_t stream[32768];
    if (!path) {
        return;
    }
    swprintf(stream, sizeof(stream) / sizeof(stream[0]), L"%ls:Zone.Identifier", path);
    DeleteFileW(stream);
}