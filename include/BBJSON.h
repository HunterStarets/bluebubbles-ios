#ifndef BB_JSON_H
#define BB_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded JSON reader. Values are offsets into the caller's buffer; nothing
 * is copied until a typed accessor is called. Nesting deeper than
 * BB_JSON_MAX_DEPTH is rejected so untrusted bodies cannot exhaust the
 * stack. */

#define BB_JSON_MAX_DEPTH 32U

typedef enum {
    BBJSONTypeInvalid = 0,
    BBJSONTypeObject,
    BBJSONTypeArray,
    BBJSONTypeString,
    BBJSONTypeNumber,
    BBJSONTypeTrue,
    BBJSONTypeFalse,
    BBJSONTypeNull
} BBJSONType;

typedef struct {
    BBJSONType type;
    size_t offset;     /* first byte of the value (the quote for strings) */
    size_t length;     /* bytes including quotes/brackets */
} BBJSONValue;

/* Parse and validate one complete JSON text. Trailing whitespace is
 * allowed; anything else after the value is rejected. */
bool bb_json_parse(const char *json, size_t length, BBJSONValue *out);

/* Object member lookup by exact (unescaped) key. The first matching member
 * wins. Returns false when the value is not an object or the key is absent. */
bool bb_json_object_get(const char *json, const BBJSONValue *object,
                        const char *key, BBJSONValue *out);

/* Iterate an array. Set *cursor to 0 before the first call. */
bool bb_json_array_next(const char *json, const BBJSONValue *array,
                        size_t *cursor, BBJSONValue *out);

/* Iterate an object. Set *cursor to 0 before the first call; the key is
 * returned as a string value. */
bool bb_json_object_next(const char *json, const BBJSONValue *object,
                         size_t *cursor, BBJSONValue *key, BBJSONValue *out);

/* Copy and unescape a string value (UTF-8; \u escapes including surrogate
 * pairs are decoded). Fails when the value is not a string or does not fit. */
bool bb_json_string_copy(const char *json, const BBJSONValue *value,
                         char *out, size_t capacity, size_t *outLength);

/* Compare a string value with an unescaped ASCII/UTF-8 string. */
bool bb_json_string_equals(const char *json, const BBJSONValue *value,
                           const char *wanted);

/* Integer numbers only: no fraction or exponent. */
bool bb_json_int64(const char *json, const BBJSONValue *value, int64_t *out);

/* true/false only. */
bool bb_json_bool(const char *json, const BBJSONValue *value, bool *out);

/* True when any element of a string array equals wanted. */
bool bb_json_array_contains_string(const char *json, const BBJSONValue *array,
                                   const char *wanted);

#ifdef __cplusplus
}
#endif

#endif
