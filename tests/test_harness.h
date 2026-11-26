#pragma once

extern int th_checks;
extern int th_failures;

void th_fail(const char *file, int line, const char *expression, const char *detail);
void th_check_str(const char *file, int line, const char *actual, const char *expected);

#define CHECK(expr) \
    do { \
        th_checks++; \
        if (!(expr)) { \
            th_fail(__FILE__, __LINE__, #expr, NULL); \
        } \
    } while (0)

#define CHECK_INT(actual, expected) \
    do { \
        long long th_a = (long long)(actual); \
        long long th_e = (long long)(expected); \
        th_checks++; \
        if (th_a != th_e) { \
            th_fail(__FILE__, __LINE__, #actual " == " #expected, NULL); \
        } \
    } while (0)

#define CHECK_STR(actual, expected) \
    do { \
        th_checks++; \
        th_check_str(__FILE__, __LINE__, (actual), (expected)); \
    } while (0)

void test_util_run(void);
void test_json_run(void);
void test_version_run(void);
void test_hotkey_run(void);
void test_neturl_run(void);
void test_config_run(void);
void test_store_run(void);
void test_flow_run(void);
void test_direction_run(void);
void test_process_filter_run(void);
void test_update_run(void);