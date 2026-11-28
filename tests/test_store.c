#include "test_harness.h"

#include "config/store.h"
#include "core/fs.h"
#include "core/time.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_temp_dir(wchar_t *out, size_t cap) {
    wchar_t base[1024];
    char id[33];
    wchar_t wide_id[64];
    wchar_t name[256];
    DWORD length = GetTempPathW(sizeof(base) / sizeof(base[0]), base);
    CHECK(length > 0);
    nc_new_id(id);
    nc_write_utf8(wide_id, sizeof(wide_id) / sizeof(wide_id[0]), id);
    swprintf(name, sizeof(name) / sizeof(name[0]), L"nc-store-test-%ls", wide_id);
    CHECK(nc_path_join(out, cap, base, name));
    CHECK(nc_make_dirs(out));
}

static void remove_tree(const wchar_t *dir) {
    wchar_t pattern[1024];
    WIN32_FIND_DATAW data;
    HANDLE find;
    if (!nc_path_join(pattern, sizeof(pattern) / sizeof(pattern[0]), dir, L"*")) {
        return;
    }
    find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t path[1024];
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            if (!nc_path_join(path, sizeof(path) / sizeof(path[0]), dir, data.cFileName)) {
                continue;
            }
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(path);
            } else {
                DeleteFileW(path);
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(dir);
}

static bool write_text(const wchar_t *path, const char *text) {
    NcErr error;
    return nc_write_file(path, text, strlen(text), &error);
}

static char *read_text(const wchar_t *path) {
    void *data = NULL;
    size_t size = 0;
    NcErr error;
    char *text;
    if (!nc_read_file(path, 2 * 1024 * 1024, &data, &size, &error)) {
        return NULL;
    }
    text = (char *)malloc(size + 1);
    if (!text) {
        free(data);
        return NULL;
    }
    memcpy(text, data, size);
    text[size] = 0;
    free(data);
    return text;
}

static bool dir_has_backup(const wchar_t *dir) {
    wchar_t pattern[1024];
    WIN32_FIND_DATAW data;
    HANDLE find;
    bool found = false;
    if (!nc_path_join(pattern, sizeof(pattern) / sizeof(pattern[0]), dir, L"nc-config-backup-*")) {
        return false;
    }
    find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        found = true;
        FindClose(find);
    }
    return found;
}

typedef struct {
    wchar_t path[1024];
    int index;
} SaveContext;

static unsigned __stdcall save_thread(void *parameter) {
    SaveContext *context = (SaveContext *)parameter;
    NcStore store;
    wstr_init(&store.path);
    NcConfig config;
    NcErr error;
    nc_store_init_path(&store, context->path);
    nc_config_fresh(&config);
    config.settings.max_run_minutes = context->index;
    if (!nc_store_save(&store, &config, &error)) {
        nc_store_free(&store);
        nc_config_free(&config);
        return 1;
    }
    nc_store_free(&store);
    nc_config_free(&config);
    return 0;
}

void test_store_run(void) {
    wchar_t dir[1024];
    wchar_t path[1024];
    NcStore store;
    NcConfig config;
    char warning[2048];

    make_temp_dir(dir, sizeof(dir) / sizeof(dir[0]));
    CHECK(nc_path_join(path, sizeof(path) / sizeof(path[0]), dir, L"config.json"));
    wstr_set(&store.path, path);

    /* Broken JSON is backed up and replaced with defaults. */
    CHECK(write_text(path, "{ not json"));
    CHECK(nc_store_load(&store, &config, warning, sizeof(warning)));
    CHECK(strstr(warning, "已恢复") != NULL);
    CHECK_INT(config.profile_count, 1);
    nc_config_free(&config);
    CHECK(dir_has_backup(dir));
    remove_tree(dir);

    /* Backup failure keeps the original path untouched. */
    make_temp_dir(dir, sizeof(dir) / sizeof(dir[0]));
    CHECK(nc_path_join(path, sizeof(path) / sizeof(path[0]), dir, L"config.json"));
    CHECK(CreateDirectoryW(path, NULL) != 0);
    {
        wchar_t original[1024];
        CHECK(nc_path_join(original, sizeof(original) / sizeof(original[0]), path, L"original"));
        CHECK(write_text(original, "keep me"));
    }
    wstr_set(&store.path, path);
    CHECK(nc_store_load(&store, &config, warning, sizeof(warning)));
    CHECK(strstr(warning, "保留原文件未覆盖") != NULL);
    {
        DWORD attributes = GetFileAttributesW(path);
        CHECK((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    {
        wchar_t original[1024];
        CHECK(nc_path_join(original, sizeof(original) / sizeof(original[0]), path, L"original"));
        {
            char *text = read_text(original);
            CHECK(text != NULL && strcmp(text, "keep me") == 0);
            free(text);
        }
    }
    nc_config_free(&config);
    remove_tree(dir);

    /* Save/load round trip and concurrent saves. */
    make_temp_dir(dir, sizeof(dir) / sizeof(dir[0]));
    CHECK(nc_path_join(path, sizeof(path) / sizeof(path[0]), dir, L"config.json"));
    wstr_set(&store.path, path);
    {
        NcErr error;
        nc_config_fresh(&config);
        config.settings.max_queue_delay_ms = 500;
        CHECK(nc_store_save(&store, &config, &error));
        nc_config_free(&config);

        {
            SaveContext contexts[8];
            HANDLE threads[8];
            int index;
            for (index = 0; index < 8; index++) {
                nc_copy_wide(contexts[index].path, sizeof(contexts[index].path) / sizeof(contexts[index].path[0]), path);
                contexts[index].index = index;
                threads[index] = (HANDLE)_beginthreadex(NULL, 0, save_thread, &contexts[index], 0, NULL);
                CHECK(threads[index] != NULL);
            }
            for (index = 0; index < 8; index++) {
                if (threads[index]) {
                    WaitForSingleObject(threads[index], INFINITE);
                    CloseHandle(threads[index]);
                }
            }
        }

        CHECK(nc_store_load(&store, &config, warning, sizeof(warning)));
        CHECK_INT(config.settings.max_queue_delay_ms, 2000);
        CHECK_INT(config.profile_count, 1);
        nc_config_free(&config);
    }
    remove_tree(dir);

    /* Loading an unchanged config does not rewrite unknown fields. */
    make_temp_dir(dir, sizeof(dir) / sizeof(dir[0]));
    CHECK(nc_path_join(path, sizeof(path) / sizeof(path[0]), dir, L"config.json"));
    wstr_set(&store.path, path);
    {
        NcErr error;
        JsonValue *root;
        Str text;
        nc_config_fresh(&config);
        CHECK(nc_store_save(&store, &config, &error));
        nc_config_free(&config);
        {
            char *initial = read_text(path);
            CHECK(initial != NULL);
            root = json_parse_cstr(initial ? initial : "{}", &error);
            free(initial);
        }
        CHECK(root != NULL);
        json_object_set(root, "future", json_parse_cstr("{\"nested\":true}", &error));
        {
            JsonValue *settings = json_object_get(root, "settings");
            CHECK(settings != NULL);
            json_object_set(settings, "futureSetting", json_integer(42));
        }
        str_init(&text);
        CHECK(json_serialize(root, &text));
        json_free(root);
        CHECK(write_text(path, str_cstr(&text)));
        str_free(&text);

        {
            char *before = read_text(path);
            char *after;
            CHECK(before != NULL);
            CHECK(nc_store_load(&store, &config, warning, sizeof(warning)));
            CHECK(warning[0] == 0);
            nc_config_free(&config);
            after = read_text(path);
            CHECK(after != NULL);
            CHECK(before && after && strcmp(before, after) == 0);
            free(before);
            free(after);
        }
    }
    remove_tree(dir);

    /* Saving preserves unknown nested fields. */
    make_temp_dir(dir, sizeof(dir) / sizeof(dir[0]));
    CHECK(nc_path_join(path, sizeof(path) / sizeof(path[0]), dir, L"config.json"));
    wstr_set(&store.path, path);
    CHECK(write_text(path,
                     "{\"version\":1,\"future\":1,\"profiles\":[{\"id\":\"p\",\"name\":\"kept\","
                     "\"futureProfile\":2}],\"settings\":{\"futureSetting\":3}}"));
    CHECK(nc_store_load(&store, &config, warning, sizeof(warning)));
    {
        NcErr error;
        CHECK(nc_store_save(&store, &config, &error));
    }
    nc_config_free(&config);
    {
        char *saved = read_text(path);
        CHECK(saved != NULL);
        if (saved) {
            CHECK(strstr(saved, "\"future\": 1") != NULL);
            CHECK(strstr(saved, "\"futureProfile\": 2") != NULL);
            CHECK(strstr(saved, "\"futureSetting\": 3") != NULL);
        }
        free(saved);
    }
    remove_tree(dir);

    /* Unsupported schema and oversize files are not rewritten. */
    make_temp_dir(dir, sizeof(dir) / sizeof(dir[0]));
    CHECK(nc_path_join(path, sizeof(path) / sizeof(path[0]), dir, L"config.json"));
    wstr_set(&store.path, path);
    {
        const char *unsupported = "{\"schemaVersion\":99,\"future\":1}";
        CHECK(write_text(path, unsupported));
        CHECK(nc_store_load(&store, &config, warning, sizeof(warning)));
        CHECK(warning[0] != 0);
        nc_config_free(&config);
        {
            char *text = read_text(path);
            CHECK(text != NULL && strcmp(text, unsupported) == 0);
            free(text);
        }
    }
    {
        size_t size = 1024 * 1024 + 1;
        char *spaces = (char *)malloc(size + 1);
        char *text;
        CHECK(spaces != NULL);
        if (spaces) {
            memset(spaces, ' ', size);
            spaces[size] = 0;
            CHECK(write_text(path, spaces));
            free(spaces);
        }
        CHECK(nc_store_load(&store, &config, warning, sizeof(warning)));
        CHECK(warning[0] != 0);
        nc_config_free(&config);
        text = read_text(path);
        CHECK(text != NULL && strlen(text) == size);
        free(text);
    }
    remove_tree(dir);
    nc_store_free(&store);
}
