#include "core/hotkey.h"

#include "core/str.h"

#include <stdio.h>
#include <string.h>

bool nc_hotkey_is_modifier_key(int32_t key) {
    switch (key) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x5B:
    case 0x5C:
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA3:
    case 0xA4:
    case 0xA5:
        return true;
    default:
        return false;
    }
}

bool nc_hotkey_is_valid([[maybe_unused]] uint32_t modifiers, int32_t key) {
    if (key == 0) {
        return false;
    }
    return !nc_hotkey_is_modifier_key(key);
}

void nc_hotkey_key_name(int32_t key, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    out[0] = 0;
    switch (key) {
    case 0x08: snprintf(out, cap, "Back"); return;
    case 0x09: snprintf(out, cap, "Tab"); return;
    case 0x0C: snprintf(out, cap, "Clear"); return;
    case 0x0D: snprintf(out, cap, "Enter"); return;
    case 0x13: snprintf(out, cap, "Pause"); return;
    case 0x14: snprintf(out, cap, "Capital"); return;
    case 0x1B: snprintf(out, cap, "Esc"); return;
    case 0x20: snprintf(out, cap, "Space"); return;
    case 0x21: snprintf(out, cap, "PageUp"); return;
    case 0x22: snprintf(out, cap, "PageDown"); return;
    case 0x23: snprintf(out, cap, "End"); return;
    case 0x24: snprintf(out, cap, "Home"); return;
    case 0x25: snprintf(out, cap, "Left"); return;
    case 0x26: snprintf(out, cap, "Up"); return;
    case 0x27: snprintf(out, cap, "Right"); return;
    case 0x28: snprintf(out, cap, "Down"); return;
    case 0x2C: snprintf(out, cap, "Snapshot"); return;
    case 0x2D: snprintf(out, cap, "Insert"); return;
    case 0x2E: snprintf(out, cap, "Delete"); return;
    case 0x5B: snprintf(out, cap, "LWin"); return;
    case 0x5C: snprintf(out, cap, "RWin"); return;
    case 0x5D: snprintf(out, cap, "Apps"); return;
    case 0x6A: snprintf(out, cap, "Multiply"); return;
    case 0x6B: snprintf(out, cap, "Add"); return;
    case 0x6C: snprintf(out, cap, "Separator"); return;
    case 0x6D: snprintf(out, cap, "Subtract"); return;
    case 0x6E: snprintf(out, cap, "Decimal"); return;
    case 0x6F: snprintf(out, cap, "Divide"); return;
    case 0x90: snprintf(out, cap, "NumLock"); return;
    case 0x91: snprintf(out, cap, "Scroll"); return;
    case 0xBA: snprintf(out, cap, ";"); return;
    case 0xBB: snprintf(out, cap, "="); return;
    case 0xBC: snprintf(out, cap, ","); return;
    case 0xBD: snprintf(out, cap, "-"); return;
    case 0xBE: snprintf(out, cap, "."); return;
    case 0xBF: snprintf(out, cap, "/"); return;
    case 0xC0: snprintf(out, cap, "\x60"); return;
    case 0xDB: snprintf(out, cap, "["); return;
    case 0xDC: snprintf(out, cap, "\\"); return;
    case 0xDD: snprintf(out, cap, "]"); return;
    case 0xDE: snprintf(out, cap, "'"); return;
    default: break;
    }
    if (key >= 0x30 && key <= 0x39) {
        snprintf(out, cap, "%c", (char)key);
    } else if (key >= 0x41 && key <= 0x5A) {
        snprintf(out, cap, "%c", (char)key);
    } else if (key >= 0x60 && key <= 0x69) {
        snprintf(out, cap, "NumPad%d", key - 0x60);
    } else if (key >= 0x70 && key <= 0x87) {
        snprintf(out, cap, "F%d", key - 0x6F);
    } else {
        snprintf(out, cap, "%ld", (long)key);
    }
}

size_t nc_hotkey_format(uint32_t modifiers, int32_t key, char *out, size_t cap) {
    char name[32];
    Str text;
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = 0;
    str_init(&text);
    if (modifiers & NC_MOD_CONTROL) {
        str_add(&text, "Ctrl + ");
    }
    if (modifiers & NC_MOD_ALT) {
        str_add(&text, "Alt + ");
    }
    if (modifiers & NC_MOD_SHIFT) {
        str_add(&text, "Shift + ");
    }
    if (modifiers & NC_MOD_WIN) {
        str_add(&text, "Win + ");
    }
    nc_hotkey_key_name(key, name, sizeof(name));
    str_add(&text, name);
    if (text.len >= cap) {
        memcpy(out, str_cstr(&text), cap - 1);
        out[cap - 1] = 0;
    } else {
        memcpy(out, str_cstr(&text), text.len + 1);
    }
    {
        size_t result = text.len;
        str_free(&text);
        return result;
    }
}
