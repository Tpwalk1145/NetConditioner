#include "test_harness.h"

#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_utf( void ) {
    wchar_t *wide = nc_wide_dup("应用 ab");
    char *utf8;
    wchar_t buffer[16];
    CHECK(wide != NULL);
    if (wide) {
        CHECK_INT(wcslen(wide), 5);
        utf8 = nc_utf8_dup(wide);
        CHECK(utf8 != NULL);
        if (utf8) {
            CHECK_STR(utf8, "应用 ab");
            free(utf8);
        }
        free(wide);
    }
    memset(buffer, 0xAA, sizeof(buffer));
    CHECK(nc_write_utf8(buffer, 5, "😀"));
    CHECK_INT(buffer[0], 0xD83D);
    CHECK_INT(buffer[1], 0xDE00);
    CHECK_INT(buffer[2], 0);
    CHECK(nc_write_utf8(buffer, 16, "hello"));
}

static void test_strings( void ) {
    Str s;
    char text[] = "  value  ";
    str_init(&s);
    CHECK(str_add(&s, "abc"));
    CHECK(str_addf(&s, "-%d", 42));
    CHECK_STR(str_cstr(&s), "abc-42");
    CHECK_STR(nc_trim(text), "value");
    CHECK(nc_ieq_ascii("AbC", "aBc"));
    CHECK(nc_istarts_ascii("Hello", "he"));
    CHECK(!nc_istarts_ascii("Hello", "lo"));
    str_free(&s);
}

static void test_process_names( void ) {
    Str name;
    str_init(&name);
    nc_normalize_process_name("Chrome.EXE", &name);
    CHECK_STR(str_cstr(&name), "chrome");
    nc_normalize_process_name("C:\\Tools\\MyApp.exe", &name);
    CHECK_STR(str_cstr(&name), "myapp");
    nc_normalize_process_name("应用.EXE", &name);
    CHECK_STR(str_cstr(&name), "应用");
    nc_normalize_process_name("   ", &name);
    CHECK_STR(str_cstr(&name), "");
    nc_normalize_display_name("ééé", &name);
    CHECK_STR(str_cstr(&name), "ééé.exe");
    nc_normalize_process_name("C:\\dir\\", &name);
    CHECK_STR(str_cstr(&name), "");
    str_free(&name);
}

static void test_hash_and_numbers( void ) {
    char buffer[64];
    int32_t value = 0;
    CHECK_INT(nc_fnv1a_utf8(""), (uint64_t)14695981039346656037ull);
    CHECK_INT(nc_fnv1a_utf8("a"), (uint64_t)0xaf63dc4c8601ec8cull);
    nc_format_i32_grouped(1234567, buffer, sizeof(buffer));
    CHECK_STR(buffer, "1,234,567");
    nc_format_i32_grouped(0, buffer, sizeof(buffer));
    CHECK_STR(buffer, "0");
    nc_format_i32_grouped(-1234, buffer, sizeof(buffer));
    CHECK_STR(buffer, "-1,234");
    CHECK(nc_parse_i32_digits("12,345", 1000000, &value));
    CHECK_INT(value, 12345);
    CHECK(nc_parse_i32_digits(" 42 ", 100, &value));
    CHECK_INT(value, 42);
    CHECK(nc_parse_i32_digits("", 100, &value));
    CHECK_INT(value, 0);
    CHECK(nc_parse_i32_digits("999999999999999999999", 100, &value));
    CHECK_INT(value, 100);
    CHECK(!nc_parse_i32_digits("-1", 100, &value));
    CHECK(!nc_parse_i32_digits("1.5", 100, &value));
    CHECK(!nc_parse_i32_digits("12ms", 100, &value));
    CHECK(!nc_parse_i32_digits("１２", 100, &value));
}

static void test_paths_and_time( void ) {
    wchar_t joined[512];
    wchar_t parent[512];
    char stamp[64];
    uint64_t state = nc_random_seed();
    int i;
    uint64_t first;
    uint64_t second;
    CHECK(nc_path_join(joined, 512, L"C:\\base", L"child"));
    CHECK(wcscmp(joined, L"C:\\base\\child") == 0);
    CHECK(nc_path_join(joined, 512, L"C:\\base\\", L"child"));
    CHECK(wcscmp(joined, L"C:\\base\\child") == 0);
    CHECK(nc_path_parent(L"C:\\base\\child", parent, 512));
    CHECK(wcscmp(parent, L"C:\\base") == 0);
    CHECK(wcscmp(nc_path_file_name(L"C:\\base\\child.exe"), L"child.exe") == 0);
    nc_format_local_iso_ms(stamp, sizeof(stamp));
    CHECK(strlen(stamp) == 23);
    first = nc_monotonic_ns();
    second = nc_monotonic_ns();
    CHECK(second >= first);
    for (i = 0; i < 100; i++) {
        CHECK(nc_rand_below(&state, 10) < 10);
    }
    CHECK(nc_rand_inclusive(&state, 0) == 0);
}

static void test_new_id( void ) {
    char id[33];
    int i;
    nc_new_id(id);
    CHECK(strlen(id) == 32);
    for (i = 0; i < 32; i++) {
        CHECK((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'));
    }
    {
        char second[33];
        nc_new_id(second);
        CHECK(strcmp(id, second) != 0);
    }
}

void test_util_run(void) {
    test_utf();
    test_strings();
    test_process_names();
    test_hash_and_numbers();
    test_paths_and_time();
    test_new_id();
}