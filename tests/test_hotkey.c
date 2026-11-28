#include "test_harness.h"

#include "core/hotkey.h"

#include <string.h>

void test_hotkey_run(void) {
    char buffer[64];
    CHECK(nc_hotkey_is_valid(0, 0x77));
    CHECK(nc_hotkey_is_valid(NC_MOD_CONTROL, 0x55));
    CHECK(!nc_hotkey_is_valid(0, 0x10));
    CHECK(!nc_hotkey_is_valid(0, 0));

    nc_hotkey_format(0, 0x77, buffer, sizeof(buffer));
    CHECK_STR(buffer, "F8");
    nc_hotkey_format(NC_MOD_CONTROL | NC_MOD_ALT, 0x55, buffer, sizeof(buffer));
    CHECK_STR(buffer, "Ctrl + Alt + U");
    nc_hotkey_format(NC_MOD_CONTROL | NC_MOD_ALT | NC_MOD_SHIFT | NC_MOD_WIN, 0x30, buffer, sizeof(buffer));
    CHECK_STR(buffer, "Ctrl + Alt + Shift + Win + 0");
    nc_hotkey_format(0, 0x0D, buffer, sizeof(buffer));
    CHECK_STR(buffer, "Enter");
    nc_hotkey_format(0, 0x1B, buffer, sizeof(buffer));
    CHECK_STR(buffer, "Esc");
    nc_hotkey_format(0, 0x70, buffer, sizeof(buffer));
    CHECK_STR(buffer, "F1");
    nc_hotkey_format(0, 0x60, buffer, sizeof(buffer));
    CHECK_STR(buffer, "NumPad0");
}
