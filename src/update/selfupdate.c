#include "update/selfupdate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fs.h"
#include "core/hash.h"
#include "core/neturl.h"
#include "core/str.h"
#include "core/time.h"
#include "update/update_log.h"

typedef struct {
    WStr target;
    WStr new_file;
    wchar_t ready[256];
    DWORD parent_pid;
    bool restart;
    char sha256[65];
    uint64_t size;
} ApplyArgs;

static void apply_args_init(ApplyArgs *args) {
    memset(args, 0, sizeof(*args));
    wstr_init(&args->target);
    wstr_init(&args->new_file);
}

static void apply_args_free(ApplyArgs *args) {
    if (!args) {
        return;
    }
    wstr_free(&args->target);
    wstr_free(&args->new_file);
}

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
    apply_args_init(args);
    value = arg_value(argc, argv, L"--target");
    if (!value || !wstr_set(&args->target, value)) {
        apply_args_free(args);
        return false;
    }
    value = arg_value(argc, argv, L"--new");
    if (!value || !wstr_set(&args->new_file, value)) {
        apply_args_free(args);
        return false;
    }
    value = arg_value(argc, argv, L"--pid");
    if (!value) {
        apply_args_free(args);
        return false;
    }
    args->parent_pid = (DWORD)wcstoul(value, NULL, 10);
    value = arg_value(argc, argv, L"--sha256");
    if (!value) {
        apply_args_free(args);
        return false;
    }
    sha_utf8 = nc_utf8_dup(value);
    if (!sha_utf8) {
        apply_args_free(args);
        return false;
    }
    if (!nc_neturl_normalize_sha256(sha_utf8, normalized) || strcmp(normalized, sha_utf8) != 0) {
        free(sha_utf8);
        apply_args_free(args);
        return false;
    }
    snprintf(args->sha256, sizeof(args->sha256), "%s", normalized);
    free(sha_utf8);
    value = arg_value(argc, argv, L"--size");
    if (!value) {
        apply_args_free(args);
        return false;
    }
    args->size = _wcstoui64(value, NULL, 10);
    if (args->size == 0) {
        apply_args_free(args);
        return false;
    }
    value = arg_value(argc, argv, L"--ready");
    if (!value || !nc_copy_wide(args->ready, sizeof(args->ready) / sizeof(args->ready[0]), value)) {
        apply_args_free(args);
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
        if (start_process(wstr_cstr(&args->target))) {
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
                             size_t size, WStr *out, NcErr *err) {
    WStr path;
    wstr_init(&path);
    if (!nc_path_join_w(&path, directory, name)) {
        wstr_free(&path);
        return nc_fail(err, "更新文件路径构造失败。");
    }
    if (!nc_write_file(wstr_cstr(&path), data, size, err)) {
        wstr_free(&path);
        return false;
    }
    wstr_free(out);
    *out = path;
    return true;
}

int nc_selfupdate_run_helper(int argc, wchar_t **argv) {
    ApplyArgs args;
    WStr directory;
    WStr target_dir;
    WStr stage_path;
    WStr backup_path;
    NcErr error;
    uint64_t source_size = 0;
    char source_hash[65];
    void *source_bytes = NULL;
    size_t source_length = 0;
    void *target_bytes = NULL;
    size_t target_length = 0;
    char target_hash[65];
    char message[1200];
    int code = 0;
    if (!parse_args(argc, argv, &args)) {
        return 2;
    }
    wstr_init(&directory);
    wstr_init(&target_dir);
    wstr_init(&stage_path);
    wstr_init(&backup_path);
    signal_ready(args.ready);
    if (!nc_update_ensure_directory_w(&directory, &error)) {
        snprintf(message, sizeof(message), "自更新失败：%s", error.msg);
        code = fail_and_restart(&args, message);
        goto done;
    }
    if (!wait_parent(args.parent_pid, 30000)) {
        code = fail_and_restart(&args, "自更新失败：父进程未在期限内正常退出，未替换文件。");
        goto done;
    }
    if (!nc_sha256_file(wstr_cstr(&args.new_file), &source_size, source_hash, &error)) {
        snprintf(message, sizeof(message), "自更新失败：无法校验更新包（%s）。", error.msg);
        code = fail_and_restart(&args, message);
        goto done;
    }
    if (source_size != args.size || !nc_ieq_ascii(source_hash, args.sha256)) {
        code = fail_and_restart(&args, "自更新失败：更新包 SHA256 或大小与父进程不一致。");
        goto done;
    }
    if (!nc_read_file(wstr_cstr(&args.new_file), 400ull * 1024ull * 1024ull, &source_bytes,
                      &source_length, &error)) {
        snprintf(message, sizeof(message), "自更新失败：无法读取更新包（%s）。", error.msg);
        code = fail_and_restart(&args, message);
        goto done;
    }
    if (!nc_path_parent_w(wstr_cstr(&args.target), &target_dir) || wstr_empty(&target_dir)) {
        wstr_set(&target_dir, L".");
    }
    if (!nc_create_temp_file_w(wstr_cstr(&target_dir), L"nc-self-stage", source_bytes, source_length,
                               &stage_path, NULL, &error)) {
        free(source_bytes);
        snprintf(message, sizeof(message), "自更新失败：无法创建暂存文件（%s）。", error.msg);
        code = fail_and_restart(&args, message);
        goto done;
    }
    {
        uint64_t staged_size = 0;
        char staged_hash[65];
        if (!nc_sha256_file(wstr_cstr(&stage_path), &staged_size, staged_hash, &error) ||
            staged_size != args.size || !nc_ieq_ascii(staged_hash, args.sha256)) {
            free(source_bytes);
            nc_remove_file(wstr_cstr(&stage_path));
            code = fail_and_restart(&args, "自更新失败：暂存文件复核失败。");
            goto done;
        }
    }
    if (!nc_read_file(wstr_cstr(&args.target), 400ull * 1024ull * 1024ull, &target_bytes,
                      &target_length, &error)) {
        free(source_bytes);
        nc_remove_file(wstr_cstr(&stage_path));
        snprintf(message, sizeof(message), "自更新失败：无法读取原程序（%s）。", error.msg);
        code = fail_and_restart(&args, message);
        goto done;
    }
    nc_sha256_hex(target_bytes, target_length, target_hash);
    if (!write_named_file(wstr_cstr(&target_dir), L"nc-self-backup.tmp", target_bytes, target_length,
                          &backup_path, &error)) {
        free(source_bytes);
        free(target_bytes);
        nc_remove_file(wstr_cstr(&stage_path));
        snprintf(message, sizeof(message), "自更新失败：无法备份原程序（%s）。", error.msg);
        code = fail_and_restart(&args, message);
        goto done;
    }
    free(target_bytes);
    free(source_bytes);
    {
        uint64_t verify_size = 0;
        char verify_hash[65];
        if (!nc_sha256_file(wstr_cstr(&args.target), &verify_size, verify_hash, &error) ||
            verify_size != target_length || !nc_ieq_ascii(verify_hash, target_hash)) {
            nc_remove_file(wstr_cstr(&stage_path));
            nc_remove_file(wstr_cstr(&backup_path));
            code = fail_and_restart(&args, "自更新失败：原程序在备份期间发生变化。");
            goto done;
        }
    }
    if (!ReplaceFileW(wstr_cstr(&args.target), wstr_cstr(&stage_path), wstr_cstr(&backup_path),
                      REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL)) {
        DWORD err_code = GetLastError();
        nc_remove_file(wstr_cstr(&stage_path));
        if (!wstr_empty(&backup_path)) {
            nc_replace_file(wstr_cstr(&backup_path), wstr_cstr(&args.target), &error);
        }
        snprintf(message, sizeof(message), "自更新失败：替换文件失败（Win32 错误 %lu）。",
                 (unsigned long)err_code);
        code = fail_and_restart(&args, message);
        goto done;
    }
    nc_selfupdate_remove_zone_identifier(wstr_cstr(&args.target));
    nc_update_log("自更新完成。");
    if (args.restart) {
        if (!start_process(wstr_cstr(&args.target))) {
            if (!wstr_empty(&backup_path)) {
                nc_replace_file(wstr_cstr(&backup_path), wstr_cstr(&args.target), &error);
            }
            nc_update_log("新版本启动失败，已回滚。");
            code = fail_and_restart(&args, "自更新已替换，但新版本启动失败。");
            goto done;
        }
        nc_update_log("已重新启动新版本。");
    }
    if (!wstr_empty(&backup_path)) {
        nc_remove_file(wstr_cstr(&backup_path));
    }
done:
    wstr_free(&backup_path);
    wstr_free(&stage_path);
    wstr_free(&target_dir);
    wstr_free(&directory);
    apply_args_free(&args);
    return code;
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
    WStr current;
    WStr directory;
    WStr helper_path;
    wchar_t helper_name[256];
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
    wstr_init(&current);
    wstr_init(&directory);
    wstr_init(&helper_path);
    if (!nc_get_module_path_w(&current)) {
        nc_err_set(err, "读取当前程序路径失败。");
        goto done;
    }
    if (!nc_update_ensure_directory_w(&directory, &local)) {
        nc_err_set(err, "%s", local.msg);
        goto done;
    }
    if (!nc_read_file(wstr_cstr(&current), 400ull * 1024ull * 1024ull, &current_bytes,
                      &current_length, &local)) {
        nc_err_set(err, "打开更新程序失败：%s", local.msg);
        goto done;
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
    if (!nc_path_join_w(&helper_path, wstr_cstr(&directory), helper_name)) {
        free(current_bytes);
        nc_err_set(err, "更新程序路径过长。");
        goto done;
    }
    {
        HANDLE handle = CreateFileW(wstr_cstr(&helper_path), GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        if (handle == INVALID_HANDLE_VALUE ||
            !WriteFile(handle, current_bytes, (DWORD)current_length, &written, NULL)) {
            DWORD code = GetLastError();
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
            free(current_bytes);
            nc_remove_file(wstr_cstr(&helper_path));
            nc_err_set(err, "准备更新程序失败（Win32 错误 %lu）。", (unsigned long)code);
            goto done;
        }
        CloseHandle(handle);
        free(current_bytes);
    }
    if (!nc_sha256_file(new_file, &payload_size, payload_hash, &local) || payload_size == 0) {
        nc_remove_file(wstr_cstr(&helper_path));
        nc_err_set(err, "校验更新包失败：%s", local.msg);
        goto done;
    }
    ready_event = CreateEventW(NULL, TRUE, FALSE, ready_name);
    if (!ready_event) {
        DWORD code = GetLastError();
        nc_remove_file(wstr_cstr(&helper_path));
        nc_err_set(err, "创建更新握手事件失败（Win32 错误 %lu）。", (unsigned long)code);
        goto done;
    }
    str_init(&command);
    append_quoted(&command, wstr_cstr(&helper_path));
    str_add(&command, " --apply-update");
    str_add(&command, " --target ");
    append_quoted(&command, wstr_cstr(&current));
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
        nc_remove_file(wstr_cstr(&helper_path));
        nc_err_set(err, "内存不足。");
        goto done;
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(wstr_cstr(&helper_path), command_wide, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) {
        DWORD code = GetLastError();
        free(command_wide);
        CloseHandle(ready_event);
        nc_remove_file(wstr_cstr(&helper_path));
        nc_err_set(err, "启动更新程序失败（Win32 错误 %lu）。", (unsigned long)code);
        goto done;
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
            nc_remove_file(wstr_cstr(&helper_path));
            nc_err_set(err, "更新程序握手超时或失败。");
            goto done;
        }
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ok = true;
done:
    wstr_free(&helper_path);
    wstr_free(&directory);
    wstr_free(&current);
    return ok;
}

void nc_selfupdate_remove_zone_identifier(const wchar_t *path) {
    WStr stream;
    if (!path || !path[0]) {
        return;
    }
    wstr_init(&stream);
    if (wstr_set(&stream, path) && wstr_add(&stream, L":Zone.Identifier")) {
        DeleteFileW(wstr_cstr(&stream));
    }
    wstr_free(&stream);
}
