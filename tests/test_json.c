#include "test_harness.h"

#include "core/json.h"

#include <stdlib.h>
#include <string.h>

static JsonValue *parse_or_null(const char *text) {
    NcErr error;
    JsonValue *value = json_parse_cstr(text, &error);
    CHECK(value != NULL);
    return value;
}

void test_json_run(void) {
    Str out;
    NcErr error;
    JsonValue *value;
    JsonValue *updated;

    value = parse_or_null("{\"b\":1,\"a\":2}");
    CHECK(value != NULL);
    if (value) {
        str_init(&out);
        CHECK(json_serialize(value, &out));
        CHECK_STR(str_cstr(&out), "{\n  \"a\": 2,\n  \"b\": 1\n}");
        str_free(&out);
        CHECK_INT(json_object_size(value), 2);
        json_free(value);
    }

    value = parse_or_null("{\"text\":\"a\\\"b\\\\c\\n\\t\\u4e2d\\ud83d\\ude00\"}");
    CHECK(value != NULL);
    if (value) {
        JsonValue *text = json_object_get(value, "text");
        CHECK(text != NULL && text->type == JSON_STRING);
        if (text) {
            CHECK(strstr(text->u.string, "中") != NULL);
            CHECK(strstr(text->u.string, "😀") != NULL);
        }
        str_init(&out);
        CHECK(json_serialize(value, &out));
        /* UTF-8 remains raw; quote, backslash, newline and tab are escaped. */
        CHECK(strstr(str_cstr(&out), "\\\"") != NULL);
        CHECK(strstr(str_cstr(&out), "\\\\") != NULL);
        CHECK(strstr(str_cstr(&out), "\\n") != NULL);
        str_free(&out);
        json_free(value);
    }

    CHECK(json_parse_cstr("{not json", &error) == NULL);
    CHECK(json_parse_cstr("{\"a\":1} trailing", &error) == NULL);
    {
        char deep[200];
        size_t index;
        for (index = 0; index < 70; index++) {
            deep[index] = '[';
        }
        for (index = 0; index < 70; index++) {
            deep[70 + index] = ']';
        }
        deep[140] = 0;
        CHECK(json_parse_cstr(deep, &error) == NULL);
    }

    /* Duplicate keys keep the last value. */
    value = parse_or_null("{\"a\":1,\"a\":2}");
    if (value) {
        JsonValue *a = json_object_get(value, "a");
        CHECK(a && a->type == JSON_NUMBER && a->u.number.integer == 2);
        json_free(value);
    }

    /* Number typing: integers are accepted, floats and out-of-range are not. */
    value = parse_or_null("{\"i\":42,\"f\":1.5,\"big\":2147483648,\"n\":null}");
    if (value) {
        int32_t number = 0;
        CHECK(json_get_i32(value, "i", &number) && number == 42);
        CHECK(!json_get_i32(value, "f", &number));
        CHECK(!json_get_i32(value, "big", &number));
        CHECK(!json_get_i32(value, "n", &number));
        CHECK(!json_get_i32(value, "missing", &number));
        json_free(value);
    }

    /* known_fields_equal ignores unknown fields but checks known ones. */
    {
        JsonValue *original = parse_or_null("{\"a\":1,\"future\":9,\"settings\":{\"x\":1,\"futureSetting\":2}}");
        JsonValue *known = parse_or_null("{\"a\":1,\"settings\":{\"x\":1}}");
        JsonValue *changed = parse_or_null("{\"a\":2,\"settings\":{\"x\":1}}");
        CHECK(json_known_fields_equal(original, known));
        CHECK(!json_known_fields_equal(original, changed));
        json_free(original);
        json_free(known);
        json_free(changed);
    }

    /* Merge keeps unknown fields and applies updated values positionally. */
    {
        JsonValue *original = parse_or_null("{\"a\":1,\"future\":9,\"array\":[1,2,3],\"settings\":{\"x\":1,\"futureSetting\":2}}");
        JsonValue *updates = parse_or_null("{\"a\":2,\"array\":[4,5],\"settings\":{\"x\":3}}");
        str_init(&out);
        json_merge_known_fields(original, updates);
        CHECK(json_serialize(original, &out));
        CHECK(strstr(str_cstr(&out), "\"a\": 2") != NULL);
        CHECK(strstr(str_cstr(&out), "\"future\": 9") != NULL);
        CHECK(strstr(str_cstr(&out), "\"futureSetting\": 2") != NULL);
        CHECK(strstr(str_cstr(&out), "\"array\"") != NULL);
        CHECK(strstr(str_cstr(&out), "\"x\": 3") != NULL);
        CHECK_INT(json_array_size(json_object_get(original, "array")), 2);
        str_free(&out);
        json_free(original);
        json_free(updates);
    }

    /* Construction helpers. */
    updated = json_object();
    CHECK(json_object_set(updated, "k", json_integer(7)));
    CHECK(json_object_set(updated, "k", json_integer(8)));
    str_init(&out);
    CHECK(json_serialize(updated, &out));
    CHECK_STR(str_cstr(&out), "{\n  \"k\": 8\n}");
    str_free(&out);
    json_free(updated);
}
