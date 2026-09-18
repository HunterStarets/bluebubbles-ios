#include "BBJSON.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static bool parse(const char *text, BBJSONValue *out)
{
    return bb_json_parse(text, strlen(text), out);
}

static void test_client_bodies(void)
{
    /* The chat query the stock client sends during full sync. */
    const char body[] =
        "{\"with\":[\"participants\",\"lastmessage\"],\"offset\":0,\"limit\":100,\"sort\":null}";
    BBJSONValue root;
    BBJSONValue value;
    int64_t number = 0;
    CHECK(parse(body, &root));
    CHECK(root.type == BBJSONTypeObject && root.offset == 0 && root.length == strlen(body));
    CHECK(bb_json_object_get(body, &root, "with", &value) && value.type == BBJSONTypeArray);
    CHECK(bb_json_array_contains_string(body, &value, "participants"));
    CHECK(bb_json_array_contains_string(body, &value, "lastmessage"));
    CHECK(!bb_json_array_contains_string(body, &value, "lastMessage"));
    CHECK(bb_json_object_get(body, &root, "offset", &value) && bb_json_int64(body, &value, &number));
    CHECK(number == 0);
    CHECK(bb_json_object_get(body, &root, "limit", &value) && bb_json_int64(body, &value, &number));
    CHECK(number == 100);
    CHECK(bb_json_object_get(body, &root, "sort", &value) && value.type == BBJSONTypeNull);
    CHECK(!bb_json_object_get(body, &root, "missing", &value));

    /* The message query with nested arrays and a large timestamp. */
    const char query[] =
        "{\"with\":[\"chats\",\"chats.participants\",\"attachments\",\"handle\"],"
        "\"where\":[],\"sort\":\"DESC\",\"after\":1758067200123,\"before\":null,"
        "\"chatGuid\":null,\"offset\":0,\"limit\":100,\"convertAttachments\":true}";
    CHECK(parse(query, &root));
    CHECK(bb_json_object_get(query, &root, "after", &value) && bb_json_int64(query, &value, &number));
    CHECK(number == INT64_C(1758067200123));
    CHECK(bb_json_object_get(query, &root, "sort", &value) && bb_json_string_equals(query, &value, "DESC"));
    CHECK(!bb_json_string_equals(query, &value, "DES"));
    CHECK(!bb_json_string_equals(query, &value, "DESCX"));
    bool flag = false;
    CHECK(bb_json_object_get(query, &root, "convertAttachments", &value) && bb_json_bool(query, &value, &flag) && flag);
    CHECK(bb_json_object_get(query, &root, "where", &value) && value.type == BBJSONTypeArray);
    size_t cursor = 0;
    BBJSONValue element;
    CHECK(!bb_json_array_next(query, &value, &cursor, &element));

    /* Object iteration order and first-match semantics for duplicate keys. */
    const char duplicate[] = "{ \"a\" : 1 , \"b\" : \"x\" , \"a\" : 2 }";
    BBJSONValue key;
    CHECK(parse(duplicate, &root));
    cursor = 0;
    CHECK(bb_json_object_next(duplicate, &root, &cursor, &key, &value));
    CHECK(bb_json_string_equals(duplicate, &key, "a") && value.type == BBJSONTypeNumber);
    CHECK(bb_json_object_next(duplicate, &root, &cursor, &key, &value));
    CHECK(bb_json_string_equals(duplicate, &key, "b") && value.type == BBJSONTypeString);
    CHECK(bb_json_object_next(duplicate, &root, &cursor, &key, &value));
    CHECK(!bb_json_object_next(duplicate, &root, &cursor, &key, &value));
    CHECK(bb_json_object_get(duplicate, &root, "a", &value) && bb_json_int64(duplicate, &value, &number) && number == 1);
}

static void test_strings_and_numbers(void)
{
    const char text[] =
        "[\"caf\\u00e9 \\ud83d\\ude00 \\\"q\\\" \\\\ \\/ \\n\\t\\b\\f\\r\", \"\", \"plain\", "
        "\"raw \xc3\xa9\", -12, 0, 9223372036854775807, -9223372036854775808, 1.5, 2e3, 9223372036854775808]";
    BBJSONValue root;
    BBJSONValue element;
    size_t cursor = 0;
    char out[64];
    size_t length = 0;
    int64_t number = 0;
    CHECK(parse(text, &root) && root.type == BBJSONTypeArray);

    CHECK(bb_json_array_next(text, &root, &cursor, &element));
    CHECK(bb_json_string_copy(text, &element, out, sizeof(out), &length));
    CHECK(strcmp(out, "caf\xc3\xa9 \xf0\x9f\x98\x80 \"q\" \\ / \n\t\b\f\r") == 0);
    CHECK(length == strlen(out));
    CHECK(!bb_json_string_copy(text, &element, out, 8, &length));
    CHECK(bb_json_array_next(text, &root, &cursor, &element));
    CHECK(bb_json_string_copy(text, &element, out, sizeof(out), &length) && length == 0 && out[0] == '\0');
    CHECK(bb_json_string_equals(text, &element, ""));
    CHECK(bb_json_array_next(text, &root, &cursor, &element));
    CHECK(bb_json_string_equals(text, &element, "plain"));
    CHECK(bb_json_array_next(text, &root, &cursor, &element));
    CHECK(bb_json_string_equals(text, &element, "raw \xc3\xa9"));

    CHECK(bb_json_array_next(text, &root, &cursor, &element) && bb_json_int64(text, &element, &number) && number == -12);
    CHECK(bb_json_array_next(text, &root, &cursor, &element) && bb_json_int64(text, &element, &number) && number == 0);
    CHECK(bb_json_array_next(text, &root, &cursor, &element) && bb_json_int64(text, &element, &number) && number == INT64_MAX);
    CHECK(bb_json_array_next(text, &root, &cursor, &element) && bb_json_int64(text, &element, &number) && number == INT64_MIN);
    CHECK(bb_json_array_next(text, &root, &cursor, &element) && element.type == BBJSONTypeNumber && !bb_json_int64(text, &element, &number));
    CHECK(bb_json_array_next(text, &root, &cursor, &element) && !bb_json_int64(text, &element, &number));
    CHECK(bb_json_array_next(text, &root, &cursor, &element) && !bb_json_int64(text, &element, &number));
    CHECK(!bb_json_array_next(text, &root, &cursor, &element));
    CHECK(!bb_json_int64(text, &root, &number));
    CHECK(!bb_json_string_copy(text, &root, out, sizeof(out), &length));
}

static void test_rejections(void)
{
    BBJSONValue root;
    const char *bad[] = {
        "", " ", "{", "}", "[1,]", "{\"a\":1,}", "{a:1}", "[1 2]", "{\"a\" 1}",
        "\"unterminated", "\"bad\\x\"", "\"\\u12\"", "\"ctl\x01\"", "01", "-", "1.", ".5",
        "1e", "+1", "tru", "nul", "[1] x", "{\"a\":1} {", "[\"a\"\"b\"]"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (parse(bad[i], &root)) {
            fprintf(stderr, "FAIL: accepted %s\n", bad[i]);
            failures++;
        }
        CHECK(root.type == BBJSONTypeInvalid);
    }

    /* Lone surrogates scan (RFC 8259 permits them) but cannot be decoded. */
    {
        const char *lone[] = { "\"\\ud83d\"", "\"\\ude00\"", "\"\\ud83d\\u0041\"" };
        char out[16];
        for (size_t i = 0; i < sizeof(lone) / sizeof(lone[0]); i++) {
            CHECK(parse(lone[i], &root) && root.type == BBJSONTypeString);
            CHECK(!bb_json_string_copy(lone[i], &root, out, sizeof(out), NULL));
            CHECK(!bb_json_string_equals(lone[i], &root, "x"));
        }
    }

    /* Depth is bounded so hostile bodies cannot exhaust the stack. */
    char deep[2 * (BB_JSON_MAX_DEPTH + 2) + 1];
    size_t cursor = 0;
    for (size_t i = 0; i < BB_JSON_MAX_DEPTH + 1; i++) deep[cursor++] = '[';
    for (size_t i = 0; i < BB_JSON_MAX_DEPTH + 1; i++) deep[cursor++] = ']';
    deep[cursor] = '\0';
    CHECK(!parse(deep, &root));
    cursor = 0;
    for (size_t i = 0; i < BB_JSON_MAX_DEPTH; i++) deep[cursor++] = '[';
    for (size_t i = 0; i < BB_JSON_MAX_DEPTH; i++) deep[cursor++] = ']';
    deep[cursor] = '\0';
    CHECK(parse(deep, &root));

    /* Whitespace variants and scalars at top level are fine. */
    CHECK(parse(" \r\n\t{ } ", &root) && root.type == BBJSONTypeObject && root.length == 3);
    CHECK(parse("null", &root) && root.type == BBJSONTypeNull);
    CHECK(parse("\"s\"", &root) && root.type == BBJSONTypeString);
    CHECK(parse("-0", &root) && root.type == BBJSONTypeNumber);
    CHECK(parse("[[[]],{\"a\":[{}]}]", &root));
    CHECK(!bb_json_parse(NULL, 4, &root));
    CHECK(!bb_json_parse("{}", 2, NULL));
}

int main(void)
{
    test_client_bodies();
    test_strings_and_numbers();
    test_rejections();
    if (failures) return 1;
    puts("json tests passed: client bodies, iteration, escapes, integers, rejections, depth");
    return 0;
}
