#include "test_harness.h"

#include <stdio.h>

static_assert(__STDC_VERSION__ >= 202311L);

int main(void) {
    long stdc_version = __STDC_VERSION__;
    CHECK(stdc_version >= 202311L);
    test_util_run();
    test_json_run();
    test_version_run();
    test_hotkey_run();
    test_neturl_run();
    test_config_run();
    test_store_run();
    test_flow_run();
    test_direction_run();
    test_process_filter_run();
    test_update_run();
    printf("Checks: %d, failures: %d\n", th_checks, th_failures);
    if (th_failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL (%d)\n", th_failures);
    return 1;
}