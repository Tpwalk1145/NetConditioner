#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define NC_MOD_ALT 0x0001u
#define NC_MOD_CONTROL 0x0002u
#define NC_MOD_SHIFT 0x0004u
#define NC_MOD_WIN 0x0008u
#define NC_MOD_NOREPEAT 0x4000u

#define NC_DEFAULT_HOTKEY_MODIFIERS (NC_MOD_CONTROL | NC_MOD_ALT)
#define NC_DEFAULT_HOTKEY_KEY 0x55

bool nc_hotkey_is_valid(uint32_t modifiers, int32_t key);
bool nc_hotkey_is_modifier_key(int32_t key);
size_t nc_hotkey_format(uint32_t modifiers, int32_t key, char *out, size_t cap);
void nc_hotkey_key_name(int32_t key, char *out, size_t cap);
