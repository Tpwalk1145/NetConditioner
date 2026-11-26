#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct {
    const char *cursor;
    const char *end;
    int depth;
    NcErr *err;
    bool failed;
} JsonParser;

static void skip_ws(JsonParser *parser) {
    while (parser->cursor < parser->end) {
        char c = *parser->cursor;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            parser->cursor++;
        } else {
            break;
        }
    }
}

static void parse_error(JsonParser *parser, const char *message) {
    if (!parser->failed) {
        parser->failed = true;
        nc_err_set(parser->err, "%s", message);
    }
}

static JsonValue *value_new(JsonType type) {
    JsonValue *value = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (value) {
        value->type = type;
    }
    return value;
}

JsonValue *json_null(void) {
    return value_new(JSON_NULL);
}

JsonValue *json_bool(bool boolean) {
    JsonValue *value = value_new(JSON_BOOL);
    if (value) {
        value->u.boolean = boolean;
    }
    return value;
}

JsonValue *json_integer(int64_t number) {
    JsonValue *value = value_new(JSON_NUMBER);
    if (value) {
        value->u.number.is_int = true;
        value->u.number.integer = number;
        value->u.number.number = (double)number;
    }
    return value;
}

JsonValue *json_string(const char *text) {
    JsonValue *value = value_new(JSON_STRING);
    if (!value) {
        return NULL;
    }
    value->u.string = nc_strdup(text ? text : "");
    if (!value->u.string) {
        free(value);
        return NULL;
    }
    return value;
}

JsonValue *json_array(void) {
    return value_new(JSON_ARRAY);
}

JsonValue *json_object(void) {
    return value_new(JSON_OBJECT);
}

void json_free(JsonValue *value) {
    size_t index;
    if (!value) {
        return;
    }
    switch (value->type) {
    case JSON_STRING:
        free(value->u.string);
        break;
    case JSON_NUMBER:
        free(value->u.number.raw);
        break;
    case JSON_ARRAY:
        for (index = 0; index < value->u.array.len; index++) {
            json_free(value->u.array.items[index]);
        }
        free(value->u.array.items);
        break;
    case JSON_OBJECT:
        for (index = 0; index < value->u.object.len; index++) {
            free(value->u.object.members[index].key);
            json_free(value->u.object.members[index].value);
        }
        free(value->u.object.members);
        break;
    default:
        break;
    }
    free(value);
}

JsonValue *json_clone(const JsonValue *value) {
    size_t index;
    JsonValue *copy;
    if (!value) {
        return NULL;
    }
    copy = value_new(value->type);
    if (!copy) {
        return NULL;
    }
    switch (value->type) {
    case JSON_BOOL:
        copy->u.boolean = value->u.boolean;
        break;
    case JSON_NUMBER:
        copy->u.number = value->u.number;
        copy->u.number.raw = value->u.number.raw ? nc_strdup(value->u.number.raw) : NULL;
        if (value->u.number.raw && !copy->u.number.raw) {
            json_free(copy);
            return NULL;
        }
        break;
    case JSON_STRING:
        copy->u.string = nc_strdup(value->u.string);
        if (!copy->u.string) {
            json_free(copy);
            return NULL;
        }
        break;
    case JSON_ARRAY:
        for (index = 0; index < value->u.array.len; index++) {
            if (!json_array_push(copy, json_clone(value->u.array.items[index]))) {
                json_free(copy);
                return NULL;
            }
        }
        break;
    case JSON_OBJECT:
        for (index = 0; index < value->u.object.len; index++) {
            JsonMember *member = &value->u.object.members[index];
            if (!json_object_set(copy, member->key, json_clone(member->value))) {
                json_free(copy);
                return NULL;
            }
        }
        break;
    default:
        break;
    }
    return copy;
}

static int member_compare(const char *left, const char *right) {
    return strcmp(left, right);
}

JsonValue *json_object_get(const JsonValue *object, const char *key) {
    size_t low = 0;
    size_t high;
    if (!object || object->type != JSON_OBJECT || !key) {
        return NULL;
    }
    high = object->u.object.len;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = member_compare(object->u.object.members[middle].key, key);
        if (order == 0) {
            return object->u.object.members[middle].value;
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

size_t json_object_size(const JsonValue *object) {
    return (object && object->type == JSON_OBJECT) ? object->u.object.len : 0;
}

JsonMember *json_object_member_at(const JsonValue *object, size_t index) {
    if (!object || object->type != JSON_OBJECT || index >= object->u.object.len) {
        return NULL;
    }
    return &object->u.object.members[index];
}

static bool object_reserve(JsonValue *object, size_t extra) {
    size_t need;
    size_t cap;
    JsonMember *members;
    if (!object || object->type != JSON_OBJECT || extra > SIZE_MAX - object->u.object.len) {
        return false;
    }
    need = object->u.object.len + extra;
    if (need <= object->u.object.cap) {
        return true;
    }
    cap = object->u.object.cap ? object->u.object.cap : 8;
    while (cap < need) {
        cap *= 2;
    }
    members = (JsonMember *)realloc(object->u.object.members, cap * sizeof(JsonMember));
    if (!members) {
        return false;
    }
    object->u.object.members = members;
    object->u.object.cap = cap;
    return true;
}

bool json_object_set(JsonValue *object, const char *key, JsonValue *value) {
    size_t low = 0;
    size_t high;
    size_t insert_at;
    char *key_copy;
    if (!object || object->type != JSON_OBJECT || !key || !value) {
        json_free(value);
        return false;
    }
    high = object->u.object.len;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = member_compare(object->u.object.members[middle].key, key);
        if (order == 0) {
            json_free(object->u.object.members[middle].value);
            object->u.object.members[middle].value = value;
            return true;
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    insert_at = low;
    key_copy = nc_strdup(key);
    if (!key_copy) {
        json_free(value);
        return false;
    }
    if (!object_reserve(object, 1)) {
        free(key_copy);
        json_free(value);
        return false;
    }
    if (insert_at < object->u.object.len) {
        memmove(&object->u.object.members[insert_at + 1], &object->u.object.members[insert_at],
                (object->u.object.len - insert_at) * sizeof(JsonMember));
    }
    object->u.object.members[insert_at].key = key_copy;
    object->u.object.members[insert_at].value = value;
    object->u.object.len++;
    return true;
}

bool json_object_remove(JsonValue *object, const char *key) {
    size_t low = 0;
    size_t high;
    if (!object || object->type != JSON_OBJECT || !key) {
        return false;
    }
    high = object->u.object.len;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = member_compare(object->u.object.members[middle].key, key);
        if (order == 0) {
            free(object->u.object.members[middle].key);
            json_free(object->u.object.members[middle].value);
            memmove(&object->u.object.members[middle], &object->u.object.members[middle + 1],
                    (object->u.object.len - middle - 1) * sizeof(JsonMember));
            object->u.object.len--;
            return true;
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

bool json_array_push(JsonValue *array, JsonValue *value) {
    size_t cap;
    JsonValue **items;
    if (!array || array->type != JSON_ARRAY || !value) {
        json_free(value);
        return false;
    }
    if (array->u.array.len == array->u.array.cap) {
        cap = array->u.array.cap ? array->u.array.cap * 2 : 8;
        items = (JsonValue **)realloc(array->u.array.items, cap * sizeof(JsonValue *));
        if (!items) {
            json_free(value);
            return false;
        }
        array->u.array.items = items;
        array->u.array.cap = cap;
    }
    array->u.array.items[array->u.array.len++] = value;
    return true;
}

size_t json_array_size(const JsonValue *array) {
    return (array && array->type == JSON_ARRAY) ? array->u.array.len : 0;
}

JsonValue *json_array_get(const JsonValue *array, size_t index) {
    if (!array || array->type != JSON_ARRAY || index >= array->u.array.len) {
        return NULL;
    }
    return array->u.array.items[index];
}

static bool append_utf8(Str *out, unsigned codepoint) {
    char bytes[4];
    size_t length;
    if (codepoint <= 0x7F) {
        bytes[0] = (char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7FF) {
        bytes[0] = (char)(0xC0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3F));
        length = 2;
    } else if (codepoint <= 0xFFFF) {
        bytes[0] = (char)(0xE0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (codepoint & 0x3F));
        length = 3;
    } else if (codepoint <= 0x10FFFF) {
        bytes[0] = (char)(0xF0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (codepoint & 0x3F));
        length = 4;
    } else {
        return false;
    }
    return str_addn(out, bytes, length);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_hex4(JsonParser *parser, unsigned *out) {
    unsigned value = 0;
    int index;
    if (parser->end - parser->cursor < 4) {
        parse_error(parser, "字符串转义不完整。");
        return false;
    }
    for (index = 0; index < 4; index++) {
        int digit = hex_value(parser->cursor[index]);
        if (digit < 0) {
            parse_error(parser, "字符串转义无效。");
            return false;
        }
        value = (value << 4) | (unsigned)digit;
    }
    parser->cursor += 4;
    *out = value;
    return true;
}

static bool parse_string(JsonParser *parser, char **out) {
    Str text;
    if (parser->cursor >= parser->end || *parser->cursor != '"') {
        parse_error(parser, "缺少字符串引号。");
        return false;
    }
    parser->cursor++;
    str_init(&text);
    while (parser->cursor < parser->end) {
        unsigned char c = (unsigned char)*parser->cursor;
        if (c == '"') {
            parser->cursor++;
            if (!text.data) {
                str_add(&text, "");
            }
            *out = str_take(&text);
            if (!*out) {
                parse_error(parser, "内存不足。");
                return false;
            }
            return true;
        }
        if (c == '\\') {
            parser->cursor++;
            if (parser->cursor >= parser->end) {
                str_free(&text);
                parse_error(parser, "字符串转义不完整。");
                return false;
            }
            c = (unsigned char)*parser->cursor++;
            switch (c) {
            case '"': str_addn(&text, "\"", 1); break;
            case '\\': str_addn(&text, "\\", 1); break;
            case '/': str_addn(&text, "/", 1); break;
            case 'b': str_addn(&text, "\b", 1); break;
            case 'f': str_addn(&text, "\f", 1); break;
            case 'n': str_addn(&text, "\n", 1); break;
            case 'r': str_addn(&text, "\r", 1); break;
            case 't': str_addn(&text, "\t", 1); break;
            case 'u': {
                unsigned codepoint;
                if (!parse_hex4(parser, &codepoint)) {
                    str_free(&text);
                    return false;
                }
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                    unsigned low;
                    if (parser->end - parser->cursor < 2 || parser->cursor[0] != '\\' ||
                        parser->cursor[1] != 'u') {
                        str_free(&text);
                        parse_error(parser, "字符串代理对不完整。");
                        return false;
                    }
                    parser->cursor += 2;
                    if (!parse_hex4(parser, &low) || low < 0xDC00 || low > 0xDFFF) {
                        str_free(&text);
                        parse_error(parser, "字符串代理对无效。");
                        return false;
                    }
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                }
                if (!append_utf8(&text, codepoint)) {
                    str_free(&text);
                    parse_error(parser, "字符串转义无效。");
                    return false;
                }
                break;
            }
            default:
                str_free(&text);
                parse_error(parser, "字符串转义无效。");
                return false;
            }
            continue;
        }
        if (c < 0x20) {
            str_free(&text);
            parse_error(parser, "字符串包含控制字符。");
            return false;
        }
        str_addn(&text, (const char *)&c, 1);
        parser->cursor++;
    }
    str_free(&text);
    parse_error(parser, "字符串未结束。");
    return false;
}

static JsonValue *parse_number(JsonParser *parser) {
    const char *start = parser->cursor;
    bool is_int = true;
    char *raw;
    JsonValue *value;
    if (parser->cursor < parser->end && *parser->cursor == '-') {
        parser->cursor++;
    }
    if (parser->cursor >= parser->end) {
        parse_error(parser, "数字格式无效。");
        return NULL;
    }
    if (*parser->cursor == '0') {
        parser->cursor++;
    } else if (*parser->cursor >= '1' && *parser->cursor <= '9') {
        while (parser->cursor < parser->end && *parser->cursor >= '0' && *parser->cursor <= '9') {
            parser->cursor++;
        }
    } else {
        parse_error(parser, "数字格式无效。");
        return NULL;
    }
    if (parser->cursor < parser->end && *parser->cursor == '.') {
        is_int = false;
        parser->cursor++;
        if (parser->cursor >= parser->end || *parser->cursor < '0' || *parser->cursor > '9') {
            parse_error(parser, "数字格式无效。");
            return NULL;
        }
        while (parser->cursor < parser->end && *parser->cursor >= '0' && *parser->cursor <= '9') {
            parser->cursor++;
        }
    }
    if (parser->cursor < parser->end && (*parser->cursor == 'e' || *parser->cursor == 'E')) {
        is_int = false;
        parser->cursor++;
        if (parser->cursor < parser->end && (*parser->cursor == '+' || *parser->cursor == '-')) {
            parser->cursor++;
        }
        if (parser->cursor >= parser->end || *parser->cursor < '0' || *parser->cursor > '9') {
            parse_error(parser, "数字格式无效。");
            return NULL;
        }
        while (parser->cursor < parser->end && *parser->cursor >= '0' && *parser->cursor <= '9') {
            parser->cursor++;
        }
    }
    value = value_new(JSON_NUMBER);
    if (!value) {
        parse_error(parser, "内存不足。");
        return NULL;
    }
    raw = nc_strndup(start, (size_t)(parser->cursor - start));
    value->u.number.raw = raw;
    if (!raw) {
        json_free(value);
        parse_error(parser, "内存不足。");
        return NULL;
    }
    value->u.number.number = strtod(raw, NULL);
    value->u.number.is_int = is_int;
    if (is_int) {
        value->u.number.integer = (int64_t)_strtoi64(raw, NULL, 10);
    } else {
        value->u.number.integer = 0;
    }
    return value;
}

static JsonValue *parse_value(JsonParser *parser);

static JsonValue *parse_array(JsonParser *parser) {
    JsonValue *array = json_array();
    if (!array) {
        parse_error(parser, "内存不足。");
        return NULL;
    }
    parser->cursor++;
    skip_ws(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->cursor++;
        return array;
    }
    for (;;) {
        JsonValue *item;
        skip_ws(parser);
        item = parse_value(parser);
        if (!item) {
            json_free(array);
            return NULL;
        }
        if (!json_array_push(array, item)) {
            json_free(array);
            parse_error(parser, "内存不足。");
            return NULL;
        }
        skip_ws(parser);
        if (parser->cursor >= parser->end) {
            json_free(array);
            parse_error(parser, "数组未结束。");
            return NULL;
        }
        if (*parser->cursor == ',') {
            parser->cursor++;
            continue;
        }
        if (*parser->cursor == ']') {
            parser->cursor++;
            return array;
        }
        json_free(array);
        parse_error(parser, "数组格式无效。");
        return NULL;
    }
}

static JsonValue *parse_object(JsonParser *parser) {
    JsonValue *object = json_object();
    if (!object) {
        parse_error(parser, "内存不足。");
        return NULL;
    }
    parser->cursor++;
    skip_ws(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return object;
    }
    for (;;) {
        char *key = NULL;
        JsonValue *value;
        skip_ws(parser);
        if (!parse_string(parser, &key)) {
            json_free(object);
            return NULL;
        }
        skip_ws(parser);
        if (parser->cursor >= parser->end || *parser->cursor != ':') {
            free(key);
            json_free(object);
            parse_error(parser, "对象缺少冒号。");
            return NULL;
        }
        parser->cursor++;
        value = parse_value(parser);
        if (!value) {
            free(key);
            json_free(object);
            return NULL;
        }
        if (!json_object_set(object, key, value)) {
            free(key);
            json_free(object);
            parse_error(parser, "内存不足。");
            return NULL;
        }
        free(key);
        skip_ws(parser);
        if (parser->cursor >= parser->end) {
            json_free(object);
            parse_error(parser, "对象未结束。");
            return NULL;
        }
        if (*parser->cursor == ',') {
            parser->cursor++;
            continue;
        }
        if (*parser->cursor == '}') {
            parser->cursor++;
            return object;
        }
        json_free(object);
        parse_error(parser, "对象格式无效。");
        return NULL;
    }
}

static bool match_literal(JsonParser *parser, const char *literal) {
    size_t length = strlen(literal);
    if ((size_t)(parser->end - parser->cursor) < length) {
        return false;
    }
    if (memcmp(parser->cursor, literal, length) != 0) {
        return false;
    }
    parser->cursor += length;
    return true;
}

static JsonValue *parse_value(JsonParser *parser) {
    if (parser->depth >= JSON_MAX_DEPTH) {
        parse_error(parser, "JSON 嵌套过深。");
        return NULL;
    }
    skip_ws(parser);
    if (parser->cursor >= parser->end) {
        parse_error(parser, "JSON 内容不完整。");
        return NULL;
    }
    switch (*parser->cursor) {
    case '{':
        parser->depth++;
        {
            JsonValue *value = parse_object(parser);
            parser->depth--;
            return value;
        }
    case '[':
        parser->depth++;
        {
            JsonValue *value = parse_array(parser);
            parser->depth--;
            return value;
        }
    case '"': {
        char *text = NULL;
        JsonValue *value;
        if (!parse_string(parser, &text)) {
            return NULL;
        }
        value = value_new(JSON_STRING);
        if (!value) {
            free(text);
            parse_error(parser, "内存不足。");
            return NULL;
        }
        value->u.string = text;
        return value;
    }
    case 't':
        if (match_literal(parser, "true")) {
            return json_bool(true);
        }
        break;
    case 'f':
        if (match_literal(parser, "false")) {
            return json_bool(false);
        }
        break;
    case 'n':
        if (match_literal(parser, "null")) {
            return json_null();
        }
        break;
    default:
        if (*parser->cursor == '-' || (*parser->cursor >= '0' && *parser->cursor <= '9')) {
            return parse_number(parser);
        }
        break;
    }
    parse_error(parser, "JSON 值无效。");
    return NULL;
}

JsonValue *json_parse(const char *text, size_t length, NcErr *err) {
    JsonParser parser;
    JsonValue *value;
    if (!text) {
        nc_err_set(err, "JSON 内容为空。");
        return NULL;
    }
    parser.cursor = text;
    parser.end = text + length;
    parser.depth = 0;
    parser.err = err;
    parser.failed = false;
    value = parse_value(&parser);
    if (!value) {
        return NULL;
    }
    skip_ws(&parser);
    if (parser.cursor != parser.end) {
        json_free(value);
        nc_err_set(err, "JSON 末尾包含多余内容。");
        return NULL;
    }
    return value;
}

JsonValue *json_parse_cstr(const char *text, NcErr *err) {
    return json_parse(text, text ? strlen(text) : 0, err);
}

static bool serialize_string(const char *text, Str *out) {
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    str_add(out, "\"");
    while (*cursor) {
        unsigned char c = *cursor++;
        switch (c) {
        case '"': str_add(out, "\\\""); break;
        case '\\': str_add(out, "\\\\"); break;
        case '\b': str_add(out, "\\b"); break;
        case '\f': str_add(out, "\\f"); break;
        case '\n': str_add(out, "\\n"); break;
        case '\r': str_add(out, "\\r"); break;
        case '\t': str_add(out, "\\t"); break;
        default:
            if (c < 0x20) {
                str_addf(out, "\\u%04x", (unsigned)c);
            } else {
                str_addn(out, (const char *)&c, 1);
            }
            break;
        }
    }
    return str_add(out, "\"");
}

static bool serialize_value(const JsonValue *value, Str *out, int indent) {
    size_t index;
    int spaces;
    if (!value) {
        return str_add(out, "null");
    }
    switch (value->type) {
    case JSON_NULL:
        return str_add(out, "null");
    case JSON_BOOL:
        return str_add(out, value->u.boolean ? "true" : "false");
    case JSON_NUMBER:
        if (value->u.number.raw) {
            return str_add(out, value->u.number.raw);
        }
        if (value->u.number.is_int) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%lld", (long long)value->u.number.integer);
            return str_add(out, buffer);
        } else {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%.17g", value->u.number.number);
            return str_add(out, buffer);
        }
    case JSON_STRING:
        return serialize_string(value->u.string, out);
    case JSON_ARRAY:
        if (value->u.array.len == 0) {
            return str_add(out, "[]");
        }
        if (!str_add(out, "[\n")) {
            return false;
        }
        for (index = 0; index < value->u.array.len; index++) {
            for (spaces = 0; spaces < indent + 2; spaces++) {
                str_add(out, " ");
            }
            if (!serialize_value(value->u.array.items[index], out, indent + 2)) {
                return false;
            }
            if (index + 1 < value->u.array.len) {
                str_add(out, ",");
            }
            str_add(out, "\n");
        }
        for (spaces = 0; spaces < indent; spaces++) {
            str_add(out, " ");
        }
        return str_add(out, "]");
    case JSON_OBJECT:
        if (value->u.object.len == 0) {
            return str_add(out, "{}");
        }
        if (!str_add(out, "{\n")) {
            return false;
        }
        for (index = 0; index < value->u.object.len; index++) {
            JsonMember *member = &value->u.object.members[index];
            for (spaces = 0; spaces < indent + 2; spaces++) {
                str_add(out, " ");
            }
            serialize_string(member->key, out);
            str_add(out, ": ");
            if (!serialize_value(member->value, out, indent + 2)) {
                return false;
            }
            if (index + 1 < value->u.object.len) {
                str_add(out, ",");
            }
            str_add(out, "\n");
        }
        for (spaces = 0; spaces < indent; spaces++) {
            str_add(out, " ");
        }
        return str_add(out, "}");
    default:
        return false;
    }
}

bool json_serialize(const JsonValue *value, Str *out) {
    return value && out ? serialize_value(value, out, 0) : false;
}

static bool number_equal(const JsonValue *left, const JsonValue *right) {
    if (left->u.number.is_int != right->u.number.is_int) {
        return false;
    }
    if (left->u.number.is_int) {
        return left->u.number.integer == right->u.number.integer;
    }
    return left->u.number.number == right->u.number.number;
}

bool json_known_fields_equal(const JsonValue *original, const JsonValue *updated) {
    size_t index;
    if (!original || !updated || original->type != updated->type) {
        return false;
    }
    switch (updated->type) {
    case JSON_OBJECT:
        for (index = 0; index < updated->u.object.len; index++) {
            JsonMember *member = &updated->u.object.members[index];
            JsonValue *existing = json_object_get(original, member->key);
            if (!existing || !json_known_fields_equal(existing, member->value)) {
                return false;
            }
        }
        return true;
    case JSON_ARRAY:
        if (original->u.array.len != updated->u.array.len) {
            return false;
        }
        for (index = 0; index < updated->u.array.len; index++) {
            if (!json_known_fields_equal(original->u.array.items[index], updated->u.array.items[index])) {
                return false;
            }
        }
        return true;
    case JSON_NUMBER:
        return number_equal(original, updated);
    case JSON_STRING:
        return strcmp(original->u.string, updated->u.string) == 0;
    case JSON_BOOL:
        return original->u.boolean == updated->u.boolean;
    case JSON_NULL:
        return true;
    default:
        return false;
    }
}

static void replace_contents(JsonValue *target, const JsonValue *source) {
    JsonValue *copy = json_clone(source);
    if (!copy) {
        return;
    }
    switch (target->type) {
    case JSON_NUMBER:
        free(target->u.number.raw);
        break;
    case JSON_STRING:
        free(target->u.string);
        break;
    case JSON_ARRAY:
        {
            size_t index;
            for (index = 0; index < target->u.array.len; index++) {
                json_free(target->u.array.items[index]);
            }
            free(target->u.array.items);
        }
        break;
    case JSON_OBJECT:
        {
            size_t index;
            for (index = 0; index < target->u.object.len; index++) {
                free(target->u.object.members[index].key);
                json_free(target->u.object.members[index].value);
            }
            free(target->u.object.members);
        }
        break;
    default:
        break;
    }
    *target = *copy;
    free(copy);
}

void json_merge_known_fields(JsonValue *original, const JsonValue *updated) {
    size_t index;
    if (!original || !updated || original->type != updated->type) {
        if (original && updated) {
            replace_contents(original, updated);
        }
        return;
    }
    if (updated->type == JSON_OBJECT) {
        for (index = 0; index < updated->u.object.len; index++) {
            JsonMember *member = &updated->u.object.members[index];
            JsonValue *existing = json_object_get(original, member->key);
            if (existing) {
                json_merge_known_fields(existing, member->value);
            } else {
                json_object_set(original, member->key, json_clone(member->value));
            }
        }
        return;
    }
    if (updated->type == JSON_ARRAY) {
        while (original->u.array.len > updated->u.array.len) {
            size_t last = original->u.array.len - 1;
            json_free(original->u.array.items[last]);
            original->u.array.len--;
        }
        for (index = 0; index < updated->u.array.len; index++) {
            if (index < original->u.array.len) {
                json_merge_known_fields(original->u.array.items[index], updated->u.array.items[index]);
            } else {
                json_array_push(original, json_clone(updated->u.array.items[index]));
            }
        }
        return;
    }
    replace_contents(original, updated);
}

bool json_get_i32(const JsonValue *object, const char *key, int32_t *out) {
    JsonValue *value;
    if (!object || !key || !out) {
        return false;
    }
    value = json_object_get(object, key);
    if (!value || value->type != JSON_NUMBER || !value->u.number.is_int) {
        return false;
    }
    if (value->u.number.integer < INT32_MIN || value->u.number.integer > INT32_MAX) {
        return false;
    }
    *out = (int32_t)value->u.number.integer;
    return true;
}

bool json_get_bool(const JsonValue *object, const char *key, bool *out) {
    JsonValue *value;
    if (!object || !key || !out) {
        return false;
    }
    value = json_object_get(object, key);
    if (!value || value->type != JSON_BOOL) {
        return false;
    }
    *out = value->u.boolean;
    return true;
}

bool json_get_string(const JsonValue *object, const char *key, Str *out) {
    JsonValue *value;
    if (!object || !key || !out) {
        return false;
    }
    value = json_object_get(object, key);
    if (!value || value->type != JSON_STRING) {
        return false;
    }
    str_clear(out);
    return str_add(out, value->u.string);
}

JsonValue *json_get_array(const JsonValue *object, const char *key) {
    JsonValue *value = json_object_get(object, key);
    return (value && value->type == JSON_ARRAY) ? value : NULL;
}

JsonValue *json_get_object(const JsonValue *object, const char *key) {
    JsonValue *value = json_object_get(object, key);
    return (value && value->type == JSON_OBJECT) ? value : NULL;
}