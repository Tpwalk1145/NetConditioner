#include "core/version.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "nc_version.h"

static bool parse_parts(const char *text, NcVersion *out) {
    char buffer[128];
    size_t length;
    size_t count = 0;
    size_t index = 0;
    uint32_t numbers[4] = {0, 0, 0, 0};
    if (!text || !out) {
        return false;
    }
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        length--;
    }
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text, length);
    buffer[length] = 0;
    while (index <= length) {
        size_t start = index;
        uint64_t value = 0;
        while (index < length && buffer[index] != '.') {
            index++;
        }
        if (index == start) {
            return false;
        }
        while (start < index) {
            unsigned char c = (unsigned char)buffer[start];
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (uint64_t)(c - '0');
            if (value > 0x7FFFFFFFull) {
                return false;
            }
            start++;
        }
        if (count >= 4) {
            return false;
        }
        numbers[count++] = (uint32_t)value;
        if (index == length) {
            break;
        }
        index++;
    }
    if (count < 2 || count > 4) {
        return false;
    }
    memcpy(out->parts, numbers, sizeof(numbers));
    return true;
}

bool nc_version_parse(const char *text, NcVersion *out) {
    NcVersion parsed;
    if (!parse_parts(text, &parsed)) {
        return false;
    }
    if (out) {
        *out = parsed;
    }
    return true;
}

void nc_version_format(const NcVersion *version, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    out[0] = 0;
    if (!version) {
        return;
    }
    if (version->parts[3] > 0) {
        snprintf(out, cap, "%u.%u.%u.%u", version->parts[0], version->parts[1],
                 version->parts[2], version->parts[3]);
    } else {
        snprintf(out, cap, "%u.%u.%u", version->parts[0], version->parts[1], version->parts[2]);
    }
}

bool nc_version_normalize(const char *text, char *out, size_t cap) {
    NcVersion version;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!nc_version_parse(text, &version)) {
        return false;
    }
    nc_version_format(&version, out, cap);
    return true;
}

int nc_version_compare(const NcVersion *left, const NcVersion *right) {
    int index;
    if (!left || !right) {
        return 0;
    }
    for (index = 0; index < 4; index++) {
        if (left->parts[index] < right->parts[index]) {
            return -1;
        }
        if (left->parts[index] > right->parts[index]) {
            return 1;
        }
    }
    return 0;
}

bool nc_version_is_newer(const NcVersion *current, const NcVersion *candidate) {
    return nc_version_compare(current, candidate) < 0;
}

NcVersion nc_version_current(void) {
    NcVersion version = {{0, 0, 0, 0}};
    if (!nc_version_parse(NC_VERSION_STR, &version)) {
        version.parts[0] = 0;
        version.parts[1] = 0;
        version.parts[2] = 0;
        version.parts[3] = 0;
    }
    return version;
}

void nc_version_display(char *out, size_t cap) {
    NcVersion version = nc_version_current();
    nc_version_format(&version, out, cap);
}
