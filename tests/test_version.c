#include "test_harness.h"

#include "../src/version.h"

#include <string.h>

static void check_parse(const char *text, int count, const uint32_t *parts) {
    NcVersion version;
    int index;
    CHECK(nc_version_parse(text, &version));
    if (count >= 0) {
        for (index = 0; index < 4; index++) {
            CHECK_INT(version.parts[index], parts[index]);
        }
    }
}

void test_version_run(void) {
    static const uint32_t one_zero_one[4] = {1, 0, 1, 0};
    static const uint32_t one_zero[4] = {1, 0, 0, 0};
    static const uint32_t one_zero_zero_five[4] = {1, 0, 0, 5};
    static const uint32_t two_three_four[4] = {2, 3, 4, 0};
    char buffer[32];
    NcVersion current;
    NcVersion candidate;

    check_parse("1.0.1", 4, one_zero_one);
    check_parse("1.0", 4, one_zero);
    check_parse("1.0.0.5", 4, one_zero_zero_five);
    check_parse(" 2.3.4 ", 4, two_three_four);
    CHECK(!nc_version_parse("1.0.0-beta", NULL));
    CHECK(!nc_version_parse("1", NULL));
    CHECK(!nc_version_parse("1..2", NULL));
    CHECK(!nc_version_parse("99999999999.0", NULL));
    CHECK(!nc_version_parse("", NULL));
    CHECK(nc_version_parse("01.002.0003", NULL));

    nc_version_format(&(NcVersion){{1, 1, 0, 0}}, buffer, sizeof(buffer));
    CHECK_STR(buffer, "1.1.0");
    nc_version_format(&(NcVersion){{1, 1, 0, 7}}, buffer, sizeof(buffer));
    CHECK_STR(buffer, "1.1.0.7");
    CHECK(nc_version_normalize("1.2", buffer, sizeof(buffer)));
    CHECK_STR(buffer, "1.2.0");
    CHECK(!nc_version_normalize("x", buffer, sizeof(buffer)));

    CHECK(nc_version_parse("1.0.0.0", &current));
    CHECK(nc_version_parse("1.0.1.0", &candidate));
    CHECK(nc_version_is_newer(&current, &candidate));
    CHECK(!nc_version_is_newer(&candidate, &current));
    CHECK(!nc_version_is_newer(&candidate, &candidate));
    CHECK(nc_version_compare(&current, &candidate) < 0);

    nc_version_display(buffer, sizeof(buffer));
    CHECK(buffer[0] != 0);
}