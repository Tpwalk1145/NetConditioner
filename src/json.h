#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "util.h"

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct {
    char *key;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        struct {
            bool is_int;
            int64_t integer;
            double number;
            char *raw;
        } number;
        char *string;
        struct {
            JsonValue **items;
            size_t len;
            size_t cap;
        } array;
        struct {
            JsonMember *members;
            size_t len;
            size_t cap;
        } object;
    } u;
};

JsonValue *json_parse(const char *text, size_t length, NcErr *err);
JsonValue *json_parse_cstr(const char *text, NcErr *err);
void json_free(JsonValue *value);
JsonValue *json_clone(const JsonValue *value);

JsonValue *json_null(void);
JsonValue *json_bool(bool value);
JsonValue *json_integer(int64_t value);
JsonValue *json_string(const char *text);
JsonValue *json_array(void);
JsonValue *json_object(void);

JsonValue *json_object_get(const JsonValue *object, const char *key);
bool json_object_set(JsonValue *object, const char *key, JsonValue *value);
bool json_object_remove(JsonValue *object, const char *key);
bool json_array_push(JsonValue *array, JsonValue *value);
size_t json_array_size(const JsonValue *array);
JsonValue *json_array_get(const JsonValue *array, size_t index);
size_t json_object_size(const JsonValue *object);
JsonMember *json_object_member_at(const JsonValue *object, size_t index);

bool json_get_i32(const JsonValue *object, const char *key, int32_t *out);
bool json_get_bool(const JsonValue *object, const char *key, bool *out);
bool json_get_string(const JsonValue *object, const char *key, Str *out);
JsonValue *json_get_array(const JsonValue *object, const char *key);
JsonValue *json_get_object(const JsonValue *object, const char *key);

bool json_serialize(const JsonValue *value, Str *out);

bool json_known_fields_equal(const JsonValue *original, const JsonValue *updated);
/* Merge updated fields into original in place (original keeps unknown fields). */
void json_merge_known_fields(JsonValue *original, const JsonValue *updated);
