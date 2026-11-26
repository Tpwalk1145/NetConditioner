#include "test_harness.h"

#include <stdio.h>
#include <string.h>

int th_checks = 0;
int th_failures = 0;

void th_fail(const char *file, int line, const char *expression, const char *detail) {
    th_failures++;
    printf("FAIL %s:%d: %s", file, line, expression ? expression : "(expression)");
    if (detail) {
        printf(" (%s)", detail);
    }
    printf("\n");
}

void th_check_str(const char *file, int line, const char *actual, const char *expected) {
    const char *left = actual ? actual : "(null)";
    const char *right = expected ? expected : "(null)";
    if (strcmp(left, right) != 0) {
        th_failures++;
        printf("FAIL %s:%d: \"%s\" != \"%s\"\n", file, line, left, right);
    }
}
