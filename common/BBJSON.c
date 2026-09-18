#include "BBJSON.h"

/* Volatile loops keep the older armv7 Clang from lowering these into libc
 * calls the SpringBoard target must not import. */
static void bb_json_memory_set(void *destination, unsigned char value,
                               size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static size_t bb_json_string_length(const char *value)
{
    const volatile char *bytes = (const volatile char *)value;
    size_t length = 0;
    if (!value) return 0;
    while (bytes[length]) length++;
    return length;
}

static bool bb_json_is_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static void bb_json_skip_space(const char *json, size_t length, size_t *cursor)
{
    while (*cursor < length && bb_json_is_space(json[*cursor])) (*cursor)++;
}

static int bb_json_hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool bb_json_scan_string(const char *json, size_t length, size_t *cursor)
{
    if (*cursor >= length || json[*cursor] != '"') return false;
    (*cursor)++;
    while (*cursor < length) {
        unsigned char byte = (unsigned char)json[*cursor];
        if (byte == '"') {
            (*cursor)++;
            return true;
        }
        if (byte < 0x20) return false;
        if (byte == '\\') {
            (*cursor)++;
            if (*cursor >= length) return false;
            switch (json[*cursor]) {
                case '"': case '\\': case '/': case 'b': case 'f':
                case 'n': case 'r': case 't':
                    break;
                case 'u':
                    if (*cursor + 4 >= length) return false;
                    for (size_t i = 1; i <= 4; i++) {
                        if (bb_json_hex_value(json[*cursor + i]) < 0) return false;
                    }
                    *cursor += 4;
                    break;
                default:
                    return false;
            }
        }
        (*cursor)++;
    }
    return false;
}

static bool bb_json_scan_digits(const char *json, size_t length, size_t *cursor)
{
    size_t start = *cursor;
    while (*cursor < length && json[*cursor] >= '0' && json[*cursor] <= '9') (*cursor)++;
    return *cursor > start;
}

static bool bb_json_scan_number(const char *json, size_t length, size_t *cursor)
{
    if (*cursor < length && json[*cursor] == '-') (*cursor)++;
    if (*cursor >= length) return false;
    if (json[*cursor] == '0') {
        (*cursor)++;
    } else if (json[*cursor] >= '1' && json[*cursor] <= '9') {
        bb_json_scan_digits(json, length, cursor);
    } else {
        return false;
    }
    if (*cursor < length && json[*cursor] == '.') {
        (*cursor)++;
        if (!bb_json_scan_digits(json, length, cursor)) return false;
    }
    if (*cursor < length && (json[*cursor] == 'e' || json[*cursor] == 'E')) {
        (*cursor)++;
        if (*cursor < length && (json[*cursor] == '+' || json[*cursor] == '-')) (*cursor)++;
        if (!bb_json_scan_digits(json, length, cursor)) return false;
    }
    return true;
}

static bool bb_json_scan_literal(const char *json, size_t length, size_t *cursor,
                                 const char *literal)
{
    size_t literalLength = bb_json_string_length(literal);
    if (length - *cursor < literalLength) return false;
    for (size_t i = 0; i < literalLength; i++) {
        if (json[*cursor + i] != literal[i]) return false;
    }
    *cursor += literalLength;
    return true;
}

static bool bb_json_scan_value(const char *json, size_t length, size_t *cursor,
                               unsigned int depth, BBJSONValue *out);

static bool bb_json_scan_container(const char *json, size_t length, size_t *cursor,
                                   unsigned int depth, bool object)
{
    char close = object ? '}' : ']';
    (*cursor)++;
    bb_json_skip_space(json, length, cursor);
    if (*cursor < length && json[*cursor] == close) {
        (*cursor)++;
        return true;
    }
    for (;;) {
        BBJSONValue element;
        bb_json_skip_space(json, length, cursor);
        if (object) {
            if (!bb_json_scan_string(json, length, cursor)) return false;
            bb_json_skip_space(json, length, cursor);
            if (*cursor >= length || json[*cursor] != ':') return false;
            (*cursor)++;
        }
        if (!bb_json_scan_value(json, length, cursor, depth, &element)) return false;
        bb_json_skip_space(json, length, cursor);
        if (*cursor >= length) return false;
        if (json[*cursor] == ',') {
            (*cursor)++;
            continue;
        }
        if (json[*cursor] == close) {
            (*cursor)++;
            return true;
        }
        return false;
    }
}

static bool bb_json_scan_value(const char *json, size_t length, size_t *cursor,
                               unsigned int depth, BBJSONValue *out)
{
    size_t start;
    bb_json_skip_space(json, length, cursor);
    if (*cursor >= length) return false;
    start = *cursor;
    switch (json[*cursor]) {
        case '{':
            if (depth >= BB_JSON_MAX_DEPTH ||
                !bb_json_scan_container(json, length, cursor, depth + 1, true)) return false;
            out->type = BBJSONTypeObject;
            break;
        case '[':
            if (depth >= BB_JSON_MAX_DEPTH ||
                !bb_json_scan_container(json, length, cursor, depth + 1, false)) return false;
            out->type = BBJSONTypeArray;
            break;
        case '"':
            if (!bb_json_scan_string(json, length, cursor)) return false;
            out->type = BBJSONTypeString;
            break;
        case 't':
            if (!bb_json_scan_literal(json, length, cursor, "true")) return false;
            out->type = BBJSONTypeTrue;
            break;
        case 'f':
            if (!bb_json_scan_literal(json, length, cursor, "false")) return false;
            out->type = BBJSONTypeFalse;
            break;
        case 'n':
            if (!bb_json_scan_literal(json, length, cursor, "null")) return false;
            out->type = BBJSONTypeNull;
            break;
        default:
            if (!bb_json_scan_number(json, length, cursor)) return false;
            out->type = BBJSONTypeNumber;
            break;
    }
    out->offset = start;
    out->length = *cursor - start;
    return true;
}

bool bb_json_parse(const char *json, size_t length, BBJSONValue *out)
{
    size_t cursor = 0;
    if (!out) return false;
    bb_json_memory_set(out, 0, sizeof(*out));
    if (!json || !length) return false;
    if (!bb_json_scan_value(json, length, &cursor, 0, out)) {
        out->type = BBJSONTypeInvalid;
        return false;
    }
    bb_json_skip_space(json, length, &cursor);
    if (cursor != length) {
        out->type = BBJSONTypeInvalid;
        return false;
    }
    return true;
}

/* Decode one escaped or literal byte sequence of a validated string,
 * producing UTF-8 into out (at most 4 bytes). */
static bool bb_json_next_code_unit(const char *json, size_t end, size_t *cursor,
                                   unsigned char *out, size_t *outLength)
{
    unsigned char byte = (unsigned char)json[*cursor];
    if (byte != '\\') {
        out[0] = byte;
        *outLength = 1;
        *cursor += 1;
        return true;
    }
    *cursor += 1;
    if (*cursor >= end) return false;
    byte = (unsigned char)json[*cursor];
    *cursor += 1;
    *outLength = 1;
    switch (byte) {
        case '"': case '\\': case '/': out[0] = byte; return true;
        case 'b': out[0] = '\b'; return true;
        case 'f': out[0] = '\f'; return true;
        case 'n': out[0] = '\n'; return true;
        case 'r': out[0] = '\r'; return true;
        case 't': out[0] = '\t'; return true;
        case 'u': {
            unsigned int code = 0;
            if (*cursor + 4 > end) return false;
            for (size_t i = 0; i < 4; i++) {
                code = code * 16 + (unsigned int)bb_json_hex_value(json[*cursor + i]);
            }
            *cursor += 4;
            if (code >= 0xD800 && code <= 0xDBFF) {
                /* High surrogate: a low surrogate escape must follow. */
                unsigned int low = 0;
                if (*cursor + 6 > end || json[*cursor] != '\\' || json[*cursor + 1] != 'u')
                    return false;
                for (size_t i = 0; i < 4; i++) {
                    int digit = bb_json_hex_value(json[*cursor + 2 + i]);
                    if (digit < 0) return false;
                    low = low * 16 + (unsigned int)digit;
                }
                if (low < 0xDC00 || low > 0xDFFF) return false;
                *cursor += 6;
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            } else if (code >= 0xDC00 && code <= 0xDFFF) {
                return false;
            }
            if (code < 0x80) {
                out[0] = (unsigned char)code;
                *outLength = 1;
            } else if (code < 0x800) {
                out[0] = (unsigned char)(0xC0 | (code >> 6));
                out[1] = (unsigned char)(0x80 | (code & 0x3F));
                *outLength = 2;
            } else if (code < 0x10000) {
                out[0] = (unsigned char)(0xE0 | (code >> 12));
                out[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
                out[2] = (unsigned char)(0x80 | (code & 0x3F));
                *outLength = 3;
            } else {
                out[0] = (unsigned char)(0xF0 | (code >> 18));
                out[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3F));
                out[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
                out[3] = (unsigned char)(0x80 | (code & 0x3F));
                *outLength = 4;
            }
            return true;
        }
        default:
            return false;
    }
}

bool bb_json_string_copy(const char *json, const BBJSONValue *value,
                         char *out, size_t capacity, size_t *outLength)
{
    size_t cursor;
    size_t end;
    size_t written = 0;
    if (!json || !value || !out || capacity == 0 ||
        value->type != BBJSONTypeString || value->length < 2) return false;
    cursor = value->offset + 1;
    end = value->offset + value->length - 1;
    while (cursor < end) {
        unsigned char unit[4];
        size_t unitLength = 0;
        if (!bb_json_next_code_unit(json, end, &cursor, unit, &unitLength)) return false;
        if (unitLength > capacity - 1 - written) return false;
        for (size_t i = 0; i < unitLength; i++) {
            if (unit[i] == 0) return false;
            out[written++] = (char)unit[i];
        }
    }
    out[written] = '\0';
    if (outLength) *outLength = written;
    return true;
}

bool bb_json_string_equals(const char *json, const BBJSONValue *value,
                           const char *wanted)
{
    size_t cursor;
    size_t end;
    size_t wantedCursor = 0;
    size_t wantedLength = bb_json_string_length(wanted);
    if (!json || !value || !wanted || value->type != BBJSONTypeString ||
        value->length < 2) return false;
    cursor = value->offset + 1;
    end = value->offset + value->length - 1;
    while (cursor < end) {
        unsigned char unit[4];
        size_t unitLength = 0;
        if (!bb_json_next_code_unit(json, end, &cursor, unit, &unitLength)) return false;
        for (size_t i = 0; i < unitLength; i++) {
            if (wantedCursor >= wantedLength ||
                (unsigned char)wanted[wantedCursor] != unit[i]) return false;
            wantedCursor++;
        }
    }
    return wantedCursor == wantedLength;
}

bool bb_json_object_next(const char *json, const BBJSONValue *object,
                         size_t *cursor, BBJSONValue *key, BBJSONValue *out)
{
    size_t position;
    size_t end;
    if (!json || !object || !cursor || !key || !out ||
        object->type != BBJSONTypeObject) return false;
    end = object->offset + object->length - 1;
    position = *cursor == 0 ? object->offset + 1 : *cursor;
    bb_json_skip_space(json, end, &position);
    if (position >= end) return false;
    if (json[position] == ',') {
        position++;
        bb_json_skip_space(json, end, &position);
    }
    if (position >= end) return false;
    key->offset = position;
    if (!bb_json_scan_string(json, end, &position)) return false;
    key->type = BBJSONTypeString;
    key->length = position - key->offset;
    bb_json_skip_space(json, end, &position);
    if (position >= end || json[position] != ':') return false;
    position++;
    if (!bb_json_scan_value(json, end, &position, 1, out)) return false;
    *cursor = position;
    return true;
}

bool bb_json_object_get(const char *json, const BBJSONValue *object,
                        const char *key, BBJSONValue *out)
{
    size_t cursor = 0;
    BBJSONValue memberKey;
    BBJSONValue member;
    if (!key || !out) return false;
    while (bb_json_object_next(json, object, &cursor, &memberKey, &member)) {
        if (bb_json_string_equals(json, &memberKey, key)) {
            *out = member;
            return true;
        }
    }
    return false;
}

bool bb_json_array_next(const char *json, const BBJSONValue *array,
                        size_t *cursor, BBJSONValue *out)
{
    size_t position;
    size_t end;
    if (!json || !array || !cursor || !out || array->type != BBJSONTypeArray)
        return false;
    end = array->offset + array->length - 1;
    position = *cursor == 0 ? array->offset + 1 : *cursor;
    bb_json_skip_space(json, end, &position);
    if (position >= end) return false;
    if (json[position] == ',') {
        position++;
    }
    if (!bb_json_scan_value(json, end, &position, 1, out)) return false;
    *cursor = position;
    return true;
}

bool bb_json_int64(const char *json, const BBJSONValue *value, int64_t *out)
{
    size_t cursor;
    size_t end;
    bool negative = false;
    uint64_t magnitude = 0;
    if (!json || !value || !out || value->type != BBJSONTypeNumber) return false;
    cursor = value->offset;
    end = value->offset + value->length;
    if (json[cursor] == '-') {
        negative = true;
        cursor++;
    }
    for (; cursor < end; cursor++) {
        unsigned int digit;
        if (json[cursor] < '0' || json[cursor] > '9') return false;
        digit = (unsigned int)(json[cursor] - '0');
        /* Bound at 2^63 without 64-bit division (no armv7 runtime helper). */
        if (magnitude > UINT64_C(922337203685477580) ||
            (magnitude == UINT64_C(922337203685477580) && digit > 8)) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (negative) {
        if (magnitude > UINT64_C(9223372036854775808)) return false;
        *out = (int64_t)(UINT64_C(0) - magnitude);
    } else {
        if (magnitude > UINT64_C(9223372036854775807)) return false;
        *out = (int64_t)magnitude;
    }
    return true;
}

bool bb_json_bool(const char *json, const BBJSONValue *value, bool *out)
{
    (void)json;
    if (!value || !out) return false;
    if (value->type == BBJSONTypeTrue) {
        *out = true;
        return true;
    }
    if (value->type == BBJSONTypeFalse) {
        *out = false;
        return true;
    }
    return false;
}

bool bb_json_array_contains_string(const char *json, const BBJSONValue *array,
                                   const char *wanted)
{
    size_t cursor = 0;
    BBJSONValue element;
    while (bb_json_array_next(json, array, &cursor, &element)) {
        if (element.type == BBJSONTypeString &&
            bb_json_string_equals(json, &element, wanted)) return true;
    }
    return false;
}
