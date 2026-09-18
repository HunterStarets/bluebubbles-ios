#include "BBRouter.h"

#include "BBJSON.h"
#include "BBWebSocket.h"

/* Route handlers stay synthetic: nothing here touches IMCore, ChatKit, IPC,
 * the filesystem, or diagnostics. The connection layer supplies bytes and
 * the router answers with BlueBubbles envelopes only. */

#define BB_ROUTER_MAX_QUERY_VALUE_BYTES 64U
#define BB_ROUTER_MAX_GUID_BYTES 256U
#define BB_ROUTER_MAX_SEGMENTS 6U
#define BB_ROUTER_DEFAULT_LIMIT 100
#define BB_ROUTER_MAX_LIMIT 1000
#define BB_ROUTER_MAX_MESSAGE_BYTES 4096U
#define BB_ROUTER_MAX_TEMP_GUID_BYTES 128U
#define BB_ROUTER_MAX_ADDRESSES 8U
#define BB_ROUTER_MAX_ADDRESS_BYTES 128U

static size_t bb_router_string_length(const char *value)
{
    const volatile char *bytes = (const volatile char *)value;
    size_t length = 0;
    if (!value) return 0;
    while (bytes[length]) length++;
    return length;
}

static bool bb_router_constant_time_equal(const char *left, const char *right,
                                          size_t length)
{
    const volatile unsigned char *a = (const volatile unsigned char *)left;
    const volatile unsigned char *b = (const volatile unsigned char *)right;
    volatile unsigned char difference = 0;
    for (size_t i = 0; i < length; i++) difference |= (unsigned char)(a[i] ^ b[i]);
    return difference == 0;
}

/* Bounded copy that never lowers into libc; truncates silently. */
static void bb_router_copy_string(char *out, size_t capacity, const char *value)
{
    size_t i = 0;
    if (!capacity) return;
    if (value) {
        for (; i + 1 < capacity && value[i]; i++) out[i] = value[i];
    }
    out[i] = '\0';
}

static void bb_router_wipe(char *buffer, size_t length)
{
    volatile char *bytes = (volatile char *)buffer;
    for (size_t i = 0; i < length; i++) bytes[i] = 0;
}

typedef enum {
    BBRouterAuthMissing = 0,
    BBRouterAuthWrong,
    BBRouterAuthOK
} BBRouterAuthResult;

/* The reference middleware reads guid, then password, then token, and treats
 * an empty value as missing. */
static BBRouterAuthResult bb_router_check_credentials(const char *query,
                                                      const char *password)
{
    static const char *const aliases[] = { "guid", "password", "token" };
    char candidate[BB_ROUTER_MAX_PASSWORD_BYTES];
    size_t candidateLength = 0;
    bool found = false;
    BBRouterAuthResult result = BBRouterAuthMissing;

    if (!query || !password) return BBRouterAuthMissing;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]) && !found; i++) {
        if (bb_http_query_value(query, aliases[i], candidate, sizeof(candidate),
                                &candidateLength) && candidateLength) {
            found = true;
        }
    }
    if (found) {
        size_t passwordLength = bb_router_string_length(password);
        result = (passwordLength == candidateLength &&
                  bb_router_constant_time_equal(candidate, password, passwordLength)) ?
                 BBRouterAuthOK : BBRouterAuthWrong;
    }
    bb_router_wipe(candidate, sizeof(candidate));
    return result;
}

bool bb_router_authenticate(const char *query, const char *password)
{
    if (!password || !password[0]) return false;
    return bb_router_check_credentials(query, password) == BBRouterAuthOK;
}

static bool bb_router_path_equals(const char *path, const char *wanted)
{
    size_t length = bb_router_string_length(path);
    size_t wantedLength = bb_router_string_length(wanted);
    /* Tolerate one trailing slash the way the reference router does. */
    if (length > 1 && path[length - 1] == '/') length--;
    if (length != wantedLength) return false;
    for (size_t i = 0; i < length; i++) {
        if (path[i] != wanted[i]) return false;
    }
    return true;
}

static bool bb_router_path_has_prefix(const char *path, const char *prefix)
{
    size_t prefixLength = bb_router_string_length(prefix);
    if (bb_router_string_length(path) < prefixLength) return false;
    for (size_t i = 0; i < prefixLength; i++) {
        if (path[i] != prefix[i]) return false;
    }
    return true;
}

static bool bb_router_finish_json(BBHTTPResponse *response, char *scratch,
                                  size_t length, int status)
{
    response->status = status;
    response->contentType = BB_RESPONSE_CONTENT_TYPE_JSON;
    response->body = (const unsigned char *)scratch;
    response->bodyLength = length;
    return true;
}

static bool bb_router_error(BBHTTPResponse *response, char *scratch,
                            size_t scratchCapacity, int status,
                            const char *message, const char *type,
                            const char *detail)
{
    size_t length = 0;
    if (!bb_response_error_envelope(scratch, scratchCapacity, &length, status,
                                    message, type, detail)) return false;
    return bb_router_finish_json(response, scratch, length, status);
}

static bool bb_router_write_metadata(BBJSONWriter *writer,
                                     const BBServerMetadata *metadata)
{
    const char *computerID = metadata->computerID ? metadata->computerID :
        "bluebubbles-ios-bridge";
    const char *osVersion = metadata->osVersion ? metadata->osVersion : "9.3.5";
    const char *serverVersion = metadata->serverVersion ? metadata->serverVersion :
        "0.2.0";
    const char *proxyService = metadata->proxyService ? metadata->proxyService :
        "Direct";
    size_t addressCount = metadata->localIPv4Count;
    if (addressCount > BB_ROUTER_MAX_LOCAL_ADDRESSES)
        addressCount = BB_ROUTER_MAX_LOCAL_ADDRESSES;

    if (!bb_json_write_raw(writer, "{") ||
        !bb_json_write_key(writer, "computer_id") ||
        !bb_json_write_string(writer, computerID) ||
        !bb_json_write_raw(writer, ",") ||
        !bb_json_write_key(writer, "os_version") ||
        !bb_json_write_string(writer, osVersion) ||
        !bb_json_write_raw(writer, ",") ||
        !bb_json_write_key(writer, "server_version") ||
        !bb_json_write_string(writer, serverVersion) ||
        !bb_json_write_raw(writer, ",\"private_api\":false"
                                   ",\"helper_connected\":false,") ||
        !bb_json_write_key(writer, "proxy_service") ||
        !bb_json_write_string(writer, proxyService) ||
        !bb_json_write_raw(writer, ",\"detected_icloud\":\"\""
                                   ",\"detected_imessage\":\"\""
                                   ",\"macos_time_sync\":null"
                                   ",\"local_ipv4s\":[")) return false;
    for (size_t i = 0; i < addressCount; i++) {
        if (!metadata->localIPv4[i]) return false;
        if (i && !bb_json_write_raw(writer, ",")) return false;
        if (!bb_json_write_string(writer, metadata->localIPv4[i])) return false;
    }
    /* ios_capabilities is ignored by the stock client and records the
     * truthful iOS 9 profile for a future forked client. */
    return bb_json_write_raw(writer, "],\"local_ipv6s\":[]"
                                     ",\"ios_capabilities\":{"
                                     "\"text\":true,\"attachments\":true,"
                                     "\"groups\":true,\"reactions\":false,"
                                     "\"effects\":false,\"replies\":false,"
                                     "\"edit\":false,\"unsend\":false,"
                                     "\"scheduling\":false}}");
}

static bool bb_router_write_metadata_envelope(const BBRouterConfig *config,
                                              const char *message,
                                              char *scratch,
                                              size_t scratchCapacity,
                                              size_t *outLength)
{
    BBJSONWriter writer;
    bb_json_writer_init(&writer, scratch, scratchCapacity);
    return bb_json_write_raw(&writer, "{\"status\":200,\"message\":") &&
           bb_json_write_string(&writer, message) &&
           bb_json_write_raw(&writer, ",\"data\":") &&
           bb_router_write_metadata(&writer, &config->metadata) &&
           bb_json_write_raw(&writer, "}") &&
           bb_json_writer_finish(&writer, outLength);
}

static bool bb_router_server_info(const BBRouterConfig *config,
                                  char *scratch, size_t scratchCapacity,
                                  BBHTTPResponse *response)
{
    size_t length = 0;
    if (!bb_router_write_metadata_envelope(config, "Success", scratch,
                                           scratchCapacity, &length)) return false;
    return bb_router_finish_json(response, scratch, length, 200);
}

/* ---- Socket.IO transport routes -------------------------------------- */

static bool bb_router_finish_text(BBHTTPResponse *response, const char *body,
                                  size_t length)
{
    response->status = 200;
    response->contentType = BB_RESPONSE_CONTENT_TYPE_TEXT;
    response->body = (const unsigned char *)body;
    response->bodyLength = length;
    return true;
}

static bool bb_router_query_equals(const char *query, const char *name,
                                   const char *wanted)
{
    char value[BB_ROUTER_MAX_QUERY_VALUE_BYTES];
    size_t length = 0;
    size_t wantedLength = bb_router_string_length(wanted);
    if (!bb_http_query_value(query, name, value, sizeof(value), &length)) return false;
    if (length != wantedLength) return false;
    for (size_t i = 0; i < length; i++) {
        if (value[i] != wanted[i]) return false;
    }
    return true;
}

static uint64_t bb_router_now(const BBRouterConfig *config)
{
    return config->nowMs ? config->nowMs(config->clockContext) : 0;
}

static bool bb_router_socket_websocket(const BBRouterConfig *config,
                                       const BBHTTPRequest *request,
                                       char *scratch, size_t scratchCapacity,
                                       BBHTTPResponse *response)
{
    static const char prefix[] = "Upgrade: websocket\r\nSec-WebSocket-Accept: ";
    char accept[BB_WS_ACCEPT_KEY_BYTES];
    size_t cursor = 0;
    BBEIOSession *session;

    if (request->method != BBHTTPMethodGET || !request->upgradeWebSocket ||
        !request->webSocketVersion13 ||
        !bb_ws_accept_key(request->webSocketKey, accept, sizeof(accept))) {
        return bb_router_error(response, scratch, scratchCapacity, 400,
                               "Bad Request", "Validation Error",
                               "WebSocket upgrade requires RFC 6455 version 13 headers");
    }
    if (scratchCapacity < sizeof(prefix) + sizeof(accept) + 2) return false;
    session = bb_eio_table_take(config->sessions, BBEIOTransportWebSocket,
                                &config->sessionCallbacks, bb_router_now(config));
    if (!session) {
        return bb_router_error(response, scratch, scratchCapacity, 503,
                               "Service Unavailable", "Socket Error",
                               "No Socket.IO session slots are available");
    }
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) scratch[cursor++] = prefix[i];
    for (size_t i = 0; accept[i]; i++) scratch[cursor++] = accept[i];
    scratch[cursor++] = '\r';
    scratch[cursor++] = '\n';
    scratch[cursor] = '\0';
    response->status = 101;
    response->cors = false;
    response->extraHeaders = scratch;
    response->handlerContext = session;
    return true;
}

static bool bb_router_socket_polling(const BBRouterConfig *config,
                                     const BBHTTPRequest *request,
                                     const unsigned char *body,
                                     size_t bodyLength,
                                     char *scratch, size_t scratchCapacity,
                                     BBHTTPResponse *response)
{
    char sid[BB_EIO_SID_BYTES];
    size_t sidLength = 0;
    size_t length = 0;
    BBEIOSession *session;
    uint64_t now = bb_router_now(config);

    if (!bb_http_query_value(request->query, "sid", sid, sizeof(sid), &sidLength) ||
        sidLength == 0) {
        if (request->method != BBHTTPMethodGET) {
            return bb_router_error(response, scratch, scratchCapacity, 400,
                                   "Bad Request", "Validation Error",
                                   "Initial polling request must be GET");
        }
        session = bb_eio_table_take(config->sessions, BBEIOTransportPolling,
                                    &config->sessionCallbacks, now);
        if (!session) {
            return bb_router_error(response, scratch, scratchCapacity, 503,
                                   "Service Unavailable", "Socket Error",
                                   "No Socket.IO session slots are available");
        }
        if (!bb_eio_write_open_packet(session->sid, scratch, scratchCapacity, &length))
            return false;
        response->handlerContext = session;
        return bb_router_finish_text(response, scratch, length);
    }

    session = bb_eio_table_find(config->sessions, sid);
    if (!session || session->transport != BBEIOTransportPolling) {
        return bb_router_error(response, scratch, scratchCapacity, 400,
                               "Bad Request", "Validation Error",
                               "Unknown Engine.IO sid");
    }
    response->handlerContext = session;
    if (request->method == BBHTTPMethodPOST) {
        if (!bb_eio_session_receive_payload(session, body, bodyLength, now)) {
            return bb_router_error(response, scratch, scratchCapacity, 400,
                                   "Bad Request", "Validation Error",
                                   "Malformed Engine.IO payload");
        }
        return bb_router_finish_text(response, "ok", 2);
    }
    if (request->method == BBHTTPMethodGET) {
        if (bb_eio_session_drain(session, (unsigned char *)scratch,
                                 scratchCapacity, &length)) {
            return bb_router_finish_text(response, scratch, length);
        }
        if (session->state == BBEIOSessionStateClosed) {
            return bb_router_finish_text(response, "1", 1);
        }
        /* Nothing queued: hold the request until the session has output. */
        response->pending = true;
        return true;
    }
    return bb_router_error(response, scratch, scratchCapacity, 405,
                           "Method Not Allowed", "Validation Error",
                           "Unsupported polling method");
}

static bool bb_router_socket_io(const BBRouterConfig *config,
                                const BBHTTPRequest *request,
                                const unsigned char *body,
                                size_t bodyLength,
                                char *scratch, size_t scratchCapacity,
                                BBHTTPResponse *response)
{
    if (!config->sessions) {
        return bb_router_error(response, scratch, scratchCapacity, 404,
                               "Not Found", "Validation Error",
                               "Route not found");
    }
    if (!bb_router_query_equals(request->query, "EIO", "4")) {
        return bb_router_error(response, scratch, scratchCapacity, 400,
                               "Bad Request", "Validation Error",
                               "Engine.IO v4 required");
    }
    if (bb_router_query_equals(request->query, "transport", "websocket")) {
        return bb_router_socket_websocket(config, request, scratch,
                                          scratchCapacity, response);
    }
    if (bb_router_query_equals(request->query, "transport", "polling")) {
        return bb_router_socket_polling(config, request, body, bodyLength,
                                        scratch, scratchCapacity, response);
    }
    return bb_router_error(response, scratch, scratchCapacity, 400,
                           "Bad Request", "Validation Error",
                           "Unsupported Engine.IO transport");
}

/* ---- Bridge-backed read routes --------------------------------------- */

/* Everything a read query can carry, from either transport. */
typedef struct {
    char guid[BB_ROUTER_MAX_GUID_BYTES];
    bool hasGuid;
    int64_t offset;
    int64_t limit;
    bool hasAfter;
    int64_t after;
    bool hasBefore;
    int64_t before;
    bool sortAscending;
    bool sortByLastMessage;
    bool withParticipants;
    bool withLastMessage;
    bool withArchived;
    bool withSMS;
    bool withAttachments;
    bool withHandle;
    bool withChats;
    bool withChatParticipants;
    /* Send fields (POST /message/text). */
    char message[BB_ROUTER_MAX_MESSAGE_BYTES];
    char tempGuid[BB_ROUTER_MAX_TEMP_GUID_BYTES];
    /* New-chat fields (POST /chat/new, start-chat). tempGuid is optional
     * here and is written as null when absent. */
    char addresses[BB_ROUTER_MAX_ADDRESSES][BB_ROUTER_MAX_ADDRESS_BYTES];
    size_t addressCount;
    bool serviceSMS;
} BBRouterQuery;

static void bb_router_query_defaults(BBRouterQuery *query)
{
    volatile unsigned char *bytes = (volatile unsigned char *)query;
    for (size_t i = 0; i < sizeof(*query); i++) bytes[i] = 0;
    query->limit = BB_ROUTER_DEFAULT_LIMIT;
    /* The client needs participants to keep chats at all. */
    query->withParticipants = true;
    query->withHandle = true;
}

static bool bb_router_ascii_equal_case(const char *value, size_t length,
                                       const char *wanted)
{
    size_t i = 0;
    for (; i < length; i++) {
        unsigned char a = (unsigned char)value[i];
        unsigned char b = (unsigned char)wanted[i];
        if (!b) return false;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        if (a != b) return false;
    }
    return wanted[i] == '\0';
}

/* Apply one "with" token; unknown tokens are ignored like the reference. */
static void bb_router_apply_with(BBRouterQuery *query, const char *token,
                                 size_t length)
{
    if (bb_router_ascii_equal_case(token, length, "participants")) query->withParticipants = true;
    else if (bb_router_ascii_equal_case(token, length, "lastmessage")) query->withLastMessage = true;
    else if (bb_router_ascii_equal_case(token, length, "archived")) query->withArchived = true;
    else if (bb_router_ascii_equal_case(token, length, "sms")) query->withSMS = true;
    else if (bb_router_ascii_equal_case(token, length, "attachment") ||
             bb_router_ascii_equal_case(token, length, "attachments")) query->withAttachments = true;
    else if (bb_router_ascii_equal_case(token, length, "handle")) query->withHandle = true;
    else if (bb_router_ascii_equal_case(token, length, "chats") ||
             bb_router_ascii_equal_case(token, length, "chat")) query->withChats = true;
    else if (bb_router_ascii_equal_case(token, length, "chats.participants") ||
             bb_router_ascii_equal_case(token, length, "chat.participants")) {
        query->withChats = true;
        query->withChatParticipants = true;
    }
}

static void bb_router_apply_with_list(BBRouterQuery *query, const char *list)
{
    size_t start = 0;
    size_t i = 0;
    for (;; i++) {
        if (list[i] == ',' || list[i] == '\0') {
            size_t end = i;
            while (start < end && list[start] == ' ') start++;
            while (end > start && list[end - 1] == ' ') end--;
            if (end > start) bb_router_apply_with(query, list + start, end - start);
            if (list[i] == '\0') break;
            start = i + 1;
        }
    }
}

static bool bb_router_apply_sort(BBRouterQuery *query, const char *value,
                                 size_t length)
{
    if (bb_router_ascii_equal_case(value, length, "ASC")) {
        query->sortAscending = true;
        return true;
    }
    if (bb_router_ascii_equal_case(value, length, "DESC")) {
        query->sortAscending = false;
        return true;
    }
    if (bb_router_ascii_equal_case(value, length, "lastmessage")) {
        query->sortByLastMessage = true;
        return true;
    }
    return false;
}

typedef enum {
    BBRouterFieldAbsent = 0,
    BBRouterFieldOK,
    BBRouterFieldInvalid
} BBRouterFieldResult;

static BBRouterFieldResult bb_router_query_int(const char *queryString,
                                               const char *name,
                                               int64_t minimum, int64_t maximum,
                                               int64_t *out)
{
    char value[BB_ROUTER_MAX_QUERY_VALUE_BYTES];
    size_t length = 0;
    BBJSONValue number;
    if (!bb_http_query_value(queryString, name, value, sizeof(value), &length))
        return BBRouterFieldAbsent;
    if (!length) return BBRouterFieldAbsent;
    if (!bb_json_parse(value, length, &number) || number.type != BBJSONTypeNumber ||
        !bb_json_int64(value, &number, out) || *out < minimum || *out > maximum)
        return BBRouterFieldInvalid;
    return BBRouterFieldOK;
}

static BBRouterFieldResult bb_router_body_int(const char *json,
                                              const BBJSONValue *object,
                                              const char *key,
                                              int64_t minimum, int64_t maximum,
                                              int64_t *out)
{
    BBJSONValue value;
    if (object->type != BBJSONTypeObject ||
        !bb_json_object_get(json, object, key, &value) ||
        value.type == BBJSONTypeNull) return BBRouterFieldAbsent;
    if (!bb_json_int64(json, &value, out) || *out < minimum || *out > maximum)
        return BBRouterFieldInvalid;
    return BBRouterFieldOK;
}

static BBRouterFieldResult bb_router_body_bool(const char *json,
                                               const BBJSONValue *object,
                                               const char *key, bool *out)
{
    BBJSONValue value;
    if (object->type != BBJSONTypeObject ||
        !bb_json_object_get(json, object, key, &value) ||
        value.type == BBJSONTypeNull) return BBRouterFieldAbsent;
    return bb_json_bool(json, &value, out) ? BBRouterFieldOK : BBRouterFieldInvalid;
}

static BBRouterFieldResult bb_router_body_string(const char *json,
                                                 const BBJSONValue *object,
                                                 const char *key,
                                                 char *out, size_t capacity)
{
    BBJSONValue value;
    if (object->type != BBJSONTypeObject ||
        !bb_json_object_get(json, object, key, &value) ||
        value.type == BBJSONTypeNull) return BBRouterFieldAbsent;
    return bb_json_string_copy(json, &value, out, capacity, NULL) ?
        BBRouterFieldOK : BBRouterFieldInvalid;
}

/* Parse the JSON body into a query. An empty body means defaults. Returns
 * NULL on success or a validation message. */
static const char *bb_router_query_from_body(const unsigned char *body,
                                             size_t bodyLength,
                                             BBRouterQuery *query)
{
    const char *json = (const char *)body;
    BBJSONValue root;
    BBJSONValue value;
    char sort[32];
    BBRouterFieldResult result;

    bb_router_query_defaults(query);
    if (!bodyLength) return NULL;
    if (!bb_json_parse(json, bodyLength, &root) || root.type != BBJSONTypeObject)
        return "Malformed JSON body";

    if (bb_json_object_get(json, &root, "with", &value) && value.type != BBJSONTypeNull) {
        size_t cursor = 0;
        BBJSONValue element;
        if (value.type == BBJSONTypeString) {
            char list[256];
            if (!bb_json_string_copy(json, &value, list, sizeof(list), NULL))
                return "Invalid with";
            bb_router_apply_with_list(query, list);
        } else if (value.type == BBJSONTypeArray) {
            while (bb_json_array_next(json, &value, &cursor, &element)) {
                char token[64];
                if (element.type != BBJSONTypeString ||
                    !bb_json_string_copy(json, &element, token, sizeof(token), NULL))
                    return "Invalid with";
                bb_router_apply_with_list(query, token);
            }
        } else {
            return "Invalid with";
        }
    }
    if (bb_router_body_int(json, &root, "offset", 0, INT64_MAX, &query->offset) ==
        BBRouterFieldInvalid) return "Invalid offset";
    if (bb_router_body_int(json, &root, "limit", 1, BB_ROUTER_MAX_LIMIT, &query->limit) ==
        BBRouterFieldInvalid) return "Invalid limit";
    result = bb_router_body_int(json, &root, "after", 0, INT64_MAX, &query->after);
    if (result == BBRouterFieldInvalid) return "Invalid after";
    query->hasAfter = result == BBRouterFieldOK;
    result = bb_router_body_int(json, &root, "before", 0, INT64_MAX, &query->before);
    if (result == BBRouterFieldInvalid) return "Invalid before";
    query->hasBefore = result == BBRouterFieldOK;
    result = bb_router_body_string(json, &root, "sort", sort, sizeof(sort));
    if (result == BBRouterFieldInvalid ||
        (result == BBRouterFieldOK && !bb_router_apply_sort(query, sort,
                                                            bb_router_string_length(sort))))
        return "Invalid sort";
    result = bb_router_body_string(json, &root, "chatGuid", query->guid, sizeof(query->guid));
    if (result == BBRouterFieldInvalid) return "Invalid chatGuid";
    query->hasGuid = result == BBRouterFieldOK && query->guid[0] != '\0';
    return NULL;
}

/* Parse a POST /message/text body. Rejects options iOS 9 cannot honor
 * (effects, subject, replies, multipart parts) rather than silently
 * dropping them. Returns NULL on success or a validation message. */
static const char *bb_router_send_from_body(const unsigned char *body,
                                            size_t bodyLength, BBRouterQuery *query)
{
    const char *json = (const char *)body;
    BBJSONValue root;
    BBJSONValue value;
    size_t length = 0;

    bb_router_query_defaults(query);
    if (!bodyLength || !bb_json_parse(json, bodyLength, &root) ||
        root.type != BBJSONTypeObject) return "Malformed JSON body";

    /* Unsupported on iOS 9: refuse loudly. With the conservative profile the
     * client does not send these, but a forged request might. */
    if (bb_json_object_get(json, &root, "effectId", &value) && value.type != BBJSONTypeNull)
        return "Message effects are not supported on iOS 9";
    if (bb_json_object_get(json, &root, "subject", &value) && value.type != BBJSONTypeNull)
        return "Message subjects are not supported on iOS 9";
    if (bb_json_object_get(json, &root, "selectedMessageGuid", &value) && value.type != BBJSONTypeNull)
        return "Replies are not supported on iOS 9";
    if (bb_json_object_get(json, &root, "partIndex", &value) && value.type != BBJSONTypeNull &&
        !(value.type == BBJSONTypeNumber))
        return "Invalid partIndex";

    if (!bb_json_object_get(json, &root, "chatGuid", &value) || value.type != BBJSONTypeString ||
        !bb_json_string_copy(json, &value, query->guid, sizeof(query->guid), NULL) || !query->guid[0])
        return "Missing chatGuid";
    query->hasGuid = true;
    if (!bb_json_object_get(json, &root, "tempGuid", &value) || value.type != BBJSONTypeString ||
        !bb_json_string_copy(json, &value, query->tempGuid, sizeof(query->tempGuid), NULL) ||
        !query->tempGuid[0])
        return "Missing tempGuid";
    if (!bb_json_object_get(json, &root, "message", &value) || value.type != BBJSONTypeString ||
        !bb_json_string_copy(json, &value, query->message, sizeof(query->message), &length))
        return "Missing or oversized message";
    if (!length) return "Empty message";
    /* method (apple-script / private-api) is accepted and ignored: both route
     * to the native ChatKit composition path. */
    return NULL;
}

/* Read one address list element into the query. */
static const char *bb_router_add_address(const char *json, const BBJSONValue *value,
                                         BBRouterQuery *query)
{
    size_t length = 0;
    if (value->type != BBJSONTypeString) return "Addresses must be strings";
    if (query->addressCount >= BB_ROUTER_MAX_ADDRESSES) return "Too many addresses";
    if (!bb_json_string_copy(json, value, query->addresses[query->addressCount],
                             BB_ROUTER_MAX_ADDRESS_BYTES, &length))
        return "Address is too long";
    if (!length) return "Empty address";
    query->addressCount++;
    return NULL;
}

/* Parse the object of a POST /chat/new body or a start-chat event.
 * addressesKey is "addresses" (REST) or "participants" (socket, where a
 * single string is also accepted). A first message is required: iOS 9
 * only lists a chat once it holds a message, so an empty chat would be
 * invisible to the client. Unsupported options are refused, as for
 * sends. Returns NULL on success or a validation message. */
static const char *bb_router_new_chat_from_object(const char *json, const BBJSONValue *root,
                                                  const char *addressesKey,
                                                  BBRouterQuery *query)
{
    BBJSONValue value;
    BBJSONValue element;
    size_t cursor = 0;
    size_t length = 0;
    char service[16];
    BBRouterFieldResult result;
    const char *problem;

    if (bb_json_object_get(json, root, "effectId", &value) && value.type != BBJSONTypeNull)
        return "Message effects are not supported on iOS 9";
    if (bb_json_object_get(json, root, "subject", &value) && value.type != BBJSONTypeNull)
        return "Message subjects are not supported on iOS 9";
    if (bb_json_object_get(json, root, "attributedBody", &value) && value.type != BBJSONTypeNull)
        return "Attributed bodies are not supported on iOS 9";

    if (!bb_json_object_get(json, root, addressesKey, &value) || value.type == BBJSONTypeNull)
        return "Missing addresses";
    if (value.type == BBJSONTypeString) {
        if ((problem = bb_router_add_address(json, &value, query)) != NULL) return problem;
    } else if (value.type == BBJSONTypeArray) {
        while (bb_json_array_next(json, &value, &cursor, &element)) {
            if ((problem = bb_router_add_address(json, &element, query)) != NULL) return problem;
        }
    } else {
        return "Addresses must be an array";
    }
    if (!query->addressCount) return "Missing addresses";

    if (!bb_json_object_get(json, root, "message", &value) || value.type == BBJSONTypeNull)
        return "A message is required to create a chat on iOS 9";
    if (value.type != BBJSONTypeString ||
        !bb_json_string_copy(json, &value, query->message, sizeof(query->message), &length))
        return "Missing or oversized message";
    if (!length) return "Empty message";

    result = bb_router_body_string(json, root, "service", service, sizeof(service));
    if (result == BBRouterFieldInvalid) return "Invalid service";
    if (result == BBRouterFieldOK) {
        size_t serviceLength = bb_router_string_length(service);
        if (bb_router_ascii_equal_case(service, serviceLength, "SMS")) query->serviceSMS = true;
        else if (!bb_router_ascii_equal_case(service, serviceLength, "iMessage"))
            return "Invalid service";
    }
    result = bb_router_body_string(json, root, "tempGuid", query->tempGuid, sizeof(query->tempGuid));
    if (result == BBRouterFieldInvalid) return "Invalid tempGuid";
    /* method (apple-script / private-api) is accepted and ignored. */
    return NULL;
}

static const char *bb_router_new_chat_from_body(const unsigned char *body,
                                                size_t bodyLength, BBRouterQuery *query)
{
    const char *json = (const char *)body;
    BBJSONValue root;
    bb_router_query_defaults(query);
    if (!bodyLength || !bb_json_parse(json, bodyLength, &root) ||
        root.type != BBJSONTypeObject) return "Malformed JSON body";
    return bb_router_new_chat_from_object(json, &root, "addresses", query);
}

/* Parse query-string options (history, counts). */
static const char *bb_router_query_from_string(const char *queryString,
                                               BBRouterQuery *query)
{
    char value[256];
    size_t length = 0;
    BBRouterFieldResult result;

    bb_router_query_defaults(query);
    if (bb_http_query_value(queryString, "with", value, sizeof(value), &length) && length)
        bb_router_apply_with_list(query, value);
    if (bb_http_query_value(queryString, "sort", value, sizeof(value), &length) && length &&
        !bb_router_apply_sort(query, value, length)) return "Invalid sort";
    if (bb_router_query_int(queryString, "offset", 0, INT64_MAX, &query->offset) ==
        BBRouterFieldInvalid) return "Invalid offset";
    if (bb_router_query_int(queryString, "limit", 1, BB_ROUTER_MAX_LIMIT, &query->limit) ==
        BBRouterFieldInvalid) return "Invalid limit";
    result = bb_router_query_int(queryString, "after", 0, INT64_MAX, &query->after);
    if (result == BBRouterFieldInvalid) return "Invalid after";
    query->hasAfter = result == BBRouterFieldOK;
    result = bb_router_query_int(queryString, "before", 0, INT64_MAX, &query->before);
    if (result == BBRouterFieldInvalid) return "Invalid before";
    query->hasBefore = result == BBRouterFieldOK;
    if (bb_http_query_value(queryString, "chatGuid", query->guid, sizeof(query->guid),
                            &length) && length) query->hasGuid = true;
    return NULL;
}

/* ---- argument JSON builders (the IPC contract) ------------------------ */

static bool bb_router_write_optional_int(BBJSONWriter *writer, const char *key,
                                         bool present, int64_t value)
{
    if (!bb_json_write_key(writer, key)) return false;
    return present ? bb_json_write_signed(writer, value) : bb_json_write_null(writer);
}

static bool bb_router_write_flag(BBJSONWriter *writer, const char *key, bool value)
{
    return bb_json_write_key(writer, key) && bb_json_write_bool(writer, value);
}

static bool bb_router_write_paging(BBJSONWriter *writer, const BBRouterQuery *query)
{
    return bb_json_write_key(writer, "offset") &&
           bb_json_write_signed(writer, query->offset) &&
           bb_json_write_raw(writer, ",") &&
           bb_json_write_key(writer, "limit") &&
           bb_json_write_signed(writer, query->limit);
}

static bool bb_router_write_sort(BBJSONWriter *writer, const BBRouterQuery *query)
{
    return bb_json_write_key(writer, "sort") &&
           bb_json_write_raw(writer, query->sortAscending ? "\"ASC\"" : "\"DESC\"");
}

static bool bb_router_write_window(BBJSONWriter *writer, const BBRouterQuery *query)
{
    return bb_router_write_optional_int(writer, "after", query->hasAfter, query->after) &&
           bb_json_write_raw(writer, ",") &&
           bb_router_write_optional_int(writer, "before", query->hasBefore, query->before);
}

typedef enum {
    BBRouterOperationChatCount = 0,
    BBRouterOperationChatQuery,
    BBRouterOperationChatGet,
    BBRouterOperationChatMessages,
    BBRouterOperationMessageCount,
    BBRouterOperationMessageQuery,
    BBRouterOperationMessageGet,
    BBRouterOperationAttachmentGet,
    BBRouterOperationAttachmentDownload,
    BBRouterOperationMessageSend,
    BBRouterOperationContactQuery,
    BBRouterOperationChatNew
} BBRouterOperation;

static const char *bb_router_operation_name(BBRouterOperation operation)
{
    switch (operation) {
        case BBRouterOperationChatCount: return "chat.count";
        case BBRouterOperationChatQuery: return "chat.query";
        case BBRouterOperationChatGet: return "chat.get";
        case BBRouterOperationChatMessages: return "chat.messages";
        case BBRouterOperationMessageCount: return "message.count";
        case BBRouterOperationMessageQuery: return "message.query";
        case BBRouterOperationMessageGet: return "message.get";
        case BBRouterOperationAttachmentGet: return "attachment.get";
        case BBRouterOperationAttachmentDownload: return "attachment.download";
        case BBRouterOperationMessageSend: return "message.send";
        case BBRouterOperationContactQuery: return "contact.query";
        case BBRouterOperationChatNew: return "chat.new";
        default: return "";
    }
}

static bool bb_router_write_arguments(BBRouterOperation operation,
                                      const BBRouterQuery *query,
                                      char *out, size_t capacity, size_t *outLength)
{
    BBJSONWriter writer;
    bool ok;
    bb_json_writer_init(&writer, out, capacity);
    if (!bb_json_write_raw(&writer, "{")) return false;
    switch (operation) {
        case BBRouterOperationChatCount:
            ok = bb_router_write_flag(&writer, "includeArchived", query->withArchived);
            break;
        case BBRouterOperationChatQuery:
            ok = bb_router_write_flag(&writer, "withParticipants", true) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withLastMessage", query->withLastMessage) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withArchived", query->withArchived) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withSMS", query->withSMS) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_json_write_key(&writer, "sort") &&
                 bb_json_write_raw(&writer, query->sortByLastMessage ? "\"lastmessage\"" : "null") &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_paging(&writer, query);
            break;
        case BBRouterOperationChatGet:
            ok = bb_json_write_key(&writer, "guid") &&
                 bb_json_write_string(&writer, query->guid) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withParticipants", true) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withLastMessage", query->withLastMessage);
            break;
        case BBRouterOperationChatMessages:
            ok = bb_json_write_key(&writer, "guid") &&
                 bb_json_write_string(&writer, query->guid) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withAttachments", query->withAttachments) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withHandle", query->withHandle) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_sort(&writer, query) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_window(&writer, query) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_paging(&writer, query);
            break;
        case BBRouterOperationMessageCount:
            ok = bb_json_write_key(&writer, "chatGuid") &&
                 bb_json_write_string(&writer, query->hasGuid ? query->guid : NULL) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_window(&writer, query);
            break;
        case BBRouterOperationMessageQuery:
            ok = bb_json_write_key(&writer, "chatGuid") &&
                 bb_json_write_string(&writer, query->hasGuid ? query->guid : NULL) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withChats", query->withChats) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withChatParticipants", query->withChatParticipants) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withAttachments", query->withAttachments) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withHandle", query->withHandle) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_sort(&writer, query) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_window(&writer, query) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_paging(&writer, query);
            break;
        case BBRouterOperationMessageGet:
            ok = bb_json_write_key(&writer, "guid") &&
                 bb_json_write_string(&writer, query->guid) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withChats", query->withChats) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withAttachments", query->withAttachments) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_router_write_flag(&writer, "withHandle", query->withHandle);
            break;
        case BBRouterOperationAttachmentGet:
        case BBRouterOperationAttachmentDownload:
            ok = bb_json_write_key(&writer, "guid") &&
                 bb_json_write_string(&writer, query->guid);
            break;
        case BBRouterOperationContactQuery:
            ok = true;  /* no arguments: the bridge returns all contacts */
            break;
        case BBRouterOperationChatNew:
            ok = bb_json_write_key(&writer, "addresses") && bb_json_write_raw(&writer, "[");
            for (size_t i = 0; ok && i < query->addressCount; i++) {
                ok = (i == 0 || bb_json_write_raw(&writer, ",")) &&
                     bb_json_write_string(&writer, query->addresses[i]);
            }
            ok = ok && bb_json_write_raw(&writer, "],") &&
                 bb_json_write_key(&writer, "service") &&
                 bb_json_write_string(&writer, query->serviceSMS ? "SMS" : "iMessage") &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_json_write_key(&writer, "message") &&
                 bb_json_write_string(&writer, query->message) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_json_write_key(&writer, "tempGuid") &&
                 bb_json_write_string(&writer, query->tempGuid[0] ? query->tempGuid : NULL);
            break;
        case BBRouterOperationMessageSend:
            ok = bb_json_write_key(&writer, "chatGuid") &&
                 bb_json_write_string(&writer, query->guid) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_json_write_key(&writer, "message") &&
                 bb_json_write_string(&writer, query->message) &&
                 bb_json_write_raw(&writer, ",") &&
                 bb_json_write_key(&writer, "tempGuid") &&
                 bb_json_write_string(&writer, query->tempGuid);
            break;
        default:
            return false;
    }
    return ok && bb_json_write_raw(&writer, "}") && bb_json_writer_finish(&writer, outLength);
}

/* ---- HTTP bridge routes ---------------------------------------------- */

typedef struct {
    const BBRouterConfig *config;
    const BBHTTPRequest *request;
    BBConnection *connection;
    const unsigned char *body;
    size_t bodyLength;
    char *scratch;
    size_t scratchCapacity;
    BBHTTPResponse *response;
} BBRouteContext;

static bool bb_router_context_error(BBRouteContext *context, int status,
                                    const char *message, const char *type,
                                    const char *detail)
{
    return bb_router_error(context->response, context->scratch,
                           context->scratchCapacity, status, message, type, detail);
}

static bool bb_router_validation_error(BBRouteContext *context, const char *detail)
{
    return bb_router_context_error(context, 400, "Bad Request", "Validation Error",
                                   detail);
}

/* Open a pending bridge request. The owner dispatches it from the
 * connection's pending callback once the connection is in pending state. */
static bool bb_router_bridge(BBRouteContext *context, BBRouterOperation operation,
                             const BBRouterQuery *query)
{
    char arguments[BB_REQUEST_MAX_ARGUMENTS_BYTES];
    size_t argumentsLength = 0;
    BBRequest *request;
    if (!context->config->requests || !context->connection) {
        return bb_router_context_error(context, 503, "Service Unavailable",
                                       "Server Error",
                                       "The messaging bridge is unavailable");
    }
    if (!bb_router_write_arguments(operation, query, arguments, sizeof(arguments),
                                   &argumentsLength)) {
        return bb_router_validation_error(context, "Request arguments are too large");
    }
    request = bb_request_table_open_http(context->config->requests, context->connection,
                                         bb_router_operation_name(operation),
                                         arguments, argumentsLength,
                                         bb_router_now(context->config));
    if (!request) {
        return bb_router_context_error(context, 503, "Service Unavailable",
                                       "Server Error", "Too many pending requests");
    }
    context->response->pending = true;
    context->response->handlerContext = request;
    return true;
}

/* Split "/api/v1/a/b/c" into decoded segments after the prefix. */
static bool bb_router_path_segments(const char *path,
                                    char segments[][BB_ROUTER_MAX_GUID_BYTES],
                                    size_t *count)
{
    size_t start = 8; /* strlen("/api/v1/") */
    size_t length = bb_router_string_length(path);
    *count = 0;
    if (length > 1 && path[length - 1] == '/') length--;
    while (start <= length) {
        size_t end = start;
        while (end < length && path[end] != '/') end++;
        if (end == start) return false;
        if (*count >= BB_ROUTER_MAX_SEGMENTS) return false;
        if (!bb_http_percent_decode(path + start, end - start, false,
                                    segments[*count], BB_ROUTER_MAX_GUID_BYTES, NULL))
            return false;
        (*count)++;
        if (end >= length) break;
        start = end + 1;
    }
    return *count != 0;
}

static bool bb_router_segment_is(const char *segment, const char *wanted)
{
    size_t i = 0;
    while (segment[i] && wanted[i]) {
        if (segment[i] != wanted[i]) return false;
        i++;
    }
    return segment[i] == wanted[i];
}

static bool bb_router_method_error(BBRouteContext *context)
{
    return bb_router_context_error(context, 405, "Method Not Allowed",
                                   "Validation Error",
                                   "Method not allowed for this route");
}

static bool bb_router_copy_guid(BBRouterQuery *query, const char *segment)
{
    size_t length = bb_router_string_length(segment);
    if (!length || length >= sizeof(query->guid)) return false;
    for (size_t i = 0; i <= length; i++) query->guid[i] = segment[i];
    query->hasGuid = true;
    return true;
}

static bool bb_router_api(BBRouteContext *context)
{
    char segments[BB_ROUTER_MAX_SEGMENTS][BB_ROUTER_MAX_GUID_BYTES];
    size_t count = 0;
    BBRouterQuery query;
    const char *problem;
    const BBHTTPRequest *request = context->request;
    bool get = request->method == BBHTTPMethodGET;
    bool post = request->method == BBHTTPMethodPOST;

    if (!bb_router_path_segments(request->path, segments, &count)) {
        return bb_router_context_error(context, 404, "Not Found", "Validation Error",
                                       "Route not found");
    }

    if (count == 1 && bb_router_segment_is(segments[0], "ping")) {
        size_t length = 0;
        if (!get) return bb_router_method_error(context);
        if (!bb_response_success_envelope(context->scratch, context->scratchCapacity,
                                          &length, "Ping received!", "\"pong\""))
            return false;
        return bb_router_finish_json(context->response, context->scratch, length, 200);
    }
    if (count == 2 && bb_router_segment_is(segments[0], "server") &&
        bb_router_segment_is(segments[1], "info")) {
        if (!get) return bb_router_method_error(context);
        return bb_router_server_info(context->config, context->scratch,
                                     context->scratchCapacity, context->response);
    }
    if (count == 2 && bb_router_segment_is(segments[0], "fcm") &&
        bb_router_segment_is(segments[1], "client")) {
        /* The client asks during setup; failure is non-gating on a LAN. */
        return bb_router_context_error(context, 404, "Not Found", "Server Error",
                                       "FCM is not supported by the iOS 9 bridge");
    }

    if (count >= 1 && bb_router_segment_is(segments[0], "chat")) {
        if (count == 2 && bb_router_segment_is(segments[1], "count")) {
            if (!get) return bb_router_method_error(context);
            if ((problem = bb_router_query_from_string(request->query, &query)) != NULL)
                return bb_router_validation_error(context, problem);
            return bb_router_bridge(context, BBRouterOperationChatCount, &query);
        }
        if (count == 2 && bb_router_segment_is(segments[1], "query")) {
            if (!post) return bb_router_method_error(context);
            if ((problem = bb_router_query_from_body(context->body, context->bodyLength,
                                                     &query)) != NULL)
                return bb_router_validation_error(context, problem);
            return bb_router_bridge(context, BBRouterOperationChatQuery, &query);
        }
        if (count == 2 && bb_router_segment_is(segments[1], "new")) {
            /* Creates the chat and sends its first message in one step. */
            if (!post) return bb_router_method_error(context);
            if ((problem = bb_router_new_chat_from_body(context->body, context->bodyLength,
                                                        &query)) != NULL)
                return bb_router_validation_error(context, problem);
            return bb_router_bridge(context, BBRouterOperationChatNew, &query);
        }
        if (count == 2) {
            if (!get) return bb_router_method_error(context);
            if ((problem = bb_router_query_from_string(request->query, &query)) != NULL)
                return bb_router_validation_error(context, problem);
            if (!bb_router_copy_guid(&query, segments[1]))
                return bb_router_validation_error(context, "Invalid chat guid");
            return bb_router_bridge(context, BBRouterOperationChatGet, &query);
        }
        if (count == 3 && bb_router_segment_is(segments[2], "message")) {
            if (!get) return bb_router_method_error(context);
            if ((problem = bb_router_query_from_string(request->query, &query)) != NULL)
                return bb_router_validation_error(context, problem);
            if (!bb_router_copy_guid(&query, segments[1]))
                return bb_router_validation_error(context, "Invalid chat guid");
            return bb_router_bridge(context, BBRouterOperationChatMessages, &query);
        }
    }

    if (count >= 1 && bb_router_segment_is(segments[0], "message")) {
        if (count == 2 && bb_router_segment_is(segments[1], "text")) {
            if (!post) return bb_router_method_error(context);
            if ((problem = bb_router_send_from_body(context->body, context->bodyLength,
                                                    &query)) != NULL)
                return bb_router_validation_error(context, problem);
            return bb_router_bridge(context, BBRouterOperationMessageSend, &query);
        }
        if (count == 2 && bb_router_segment_is(segments[1], "count")) {
            if (!get) return bb_router_method_error(context);
            if ((problem = bb_router_query_from_string(request->query, &query)) != NULL)
                return bb_router_validation_error(context, problem);
            return bb_router_bridge(context, BBRouterOperationMessageCount, &query);
        }
        if (count == 2 && bb_router_segment_is(segments[1], "query")) {
            if (!post) return bb_router_method_error(context);
            if ((problem = bb_router_query_from_body(context->body, context->bodyLength,
                                                     &query)) != NULL)
                return bb_router_validation_error(context, problem);
            return bb_router_bridge(context, BBRouterOperationMessageQuery, &query);
        }
        if (count == 2) {
            if (!get) return bb_router_method_error(context);
            if ((problem = bb_router_query_from_string(request->query, &query)) != NULL)
                return bb_router_validation_error(context, problem);
            if (!bb_router_copy_guid(&query, segments[1]))
                return bb_router_validation_error(context, "Invalid message guid");
            return bb_router_bridge(context, BBRouterOperationMessageGet, &query);
        }
    }

    if (bb_router_segment_is(segments[0], "contact")) {
        /* GET /contact (all, the client's fetchAll path) or
         * POST /contact/query (by addresses). v1 returns all contacts and
         * lets the client match; extraProperties=avatar is accepted. */
        if (count == 1 && get) {
            bb_router_query_defaults(&query);
            return bb_router_bridge(context, BBRouterOperationContactQuery, &query);
        }
        if (count == 2 && bb_router_segment_is(segments[1], "query") && post) {
            bb_router_query_defaults(&query);
            return bb_router_bridge(context, BBRouterOperationContactQuery, &query);
        }
        if (count >= 1) return bb_router_method_error(context);
    }

    if (count >= 2 && bb_router_segment_is(segments[0], "attachment")) {
        bb_router_query_defaults(&query);
        if (count == 2) {
            if (!get) return bb_router_method_error(context);
            if (!bb_router_copy_guid(&query, segments[1]))
                return bb_router_validation_error(context, "Invalid attachment guid");
            return bb_router_bridge(context, BBRouterOperationAttachmentGet, &query);
        }
        /* Raw bytes: the bridge returns a local path, the transport streams
         * it. The `original` query param is accepted and ignored (iOS 9 has
         * no conversion pipeline). */
        if (count == 3 && bb_router_segment_is(segments[2], "download")) {
            if (!get) return bb_router_method_error(context);
            if (!bb_router_copy_guid(&query, segments[1]))
                return bb_router_validation_error(context, "Invalid attachment guid");
            return bb_router_bridge(context, BBRouterOperationAttachmentDownload, &query);
        }
    }

    return bb_router_context_error(context, 404, "Not Found", "Validation Error",
                                   "Route not found");
}

/* ---- Socket.IO events ------------------------------------------------ */

static bool bb_router_socket_ack_error(BBEIOSession *session, uint64_t ackID,
                                       char *scratch, size_t scratchCapacity,
                                       int status, const char *message,
                                       const char *type, const char *detail)
{
    size_t length = 0;
    if (!bb_response_error_envelope(scratch, scratchCapacity, &length, status,
                                    message, type, detail)) return false;
    return bb_eio_session_ack(session, ackID, scratch, length);
}

/* Locate the second array element of an event (its arguments). Returns
 * false on a malformed event; *present is false when it is absent/null. */
static bool bb_router_event_arguments(const unsigned char *packet, const BBSIOPacket *parsed,
                                      BBJSONValue *arguments, bool *present)
{
    const char *json = (const char *)packet + parsed->jsonOffset;
    BBJSONValue array;
    BBJSONValue element;
    size_t cursor = 0;
    *present = false;
    if (!bb_json_parse(json, parsed->jsonLength, &array) || array.type != BBJSONTypeArray)
        return false;
    if (!bb_json_array_next(json, &array, &cursor, &element)) return false;
    if (!bb_json_array_next(json, &array, &cursor, arguments)) return true;
    *present = arguments->type != BBJSONTypeNull;
    return true;
}

/* start-chat: {participants: string|string[], message, service?, tempGuid?} */
static const char *bb_router_new_chat_from_event(const unsigned char *packet,
                                                 const BBSIOPacket *parsed,
                                                 BBRouterQuery *query)
{
    const char *json = (const char *)packet + parsed->jsonOffset;
    BBJSONValue arguments;
    bool present = false;
    bb_router_query_defaults(query);
    if (!bb_router_event_arguments(packet, parsed, &arguments, &present)) return "Malformed event";
    if (!present) return "Missing addresses";
    if (arguments.type != BBJSONTypeObject) return "Event arguments must be an object";
    return bb_router_new_chat_from_object(json, &arguments, "participants", query);
}

/* Read the second array element of an event as the arguments object. */
static const char *bb_router_query_from_event(const unsigned char *packet,
                                              const BBSIOPacket *parsed,
                                              BBRouterQuery *query,
                                              bool *hasArguments)
{
    const char *json = (const char *)packet + parsed->jsonOffset;
    BBJSONValue array;
    BBJSONValue element;
    BBJSONValue arguments;
    size_t cursor = 0;
    BBRouterFieldResult result;
    char text[64];

    bb_router_query_defaults(query);
    *hasArguments = false;
    if (!bb_json_parse(json, parsed->jsonLength, &array) || array.type != BBJSONTypeArray)
        return "Malformed event";
    if (!bb_json_array_next(json, &array, &cursor, &element)) return "Malformed event";
    if (!bb_json_array_next(json, &array, &cursor, &arguments)) return NULL;
    if (arguments.type == BBJSONTypeNull) return NULL;
    if (arguments.type != BBJSONTypeObject) return "Event arguments must be an object";
    *hasArguments = true;

    if (bb_router_body_int(json, &arguments, "offset", 0, INT64_MAX, &query->offset) ==
        BBRouterFieldInvalid) return "Invalid offset";
    if (bb_router_body_int(json, &arguments, "limit", 1, BB_ROUTER_MAX_LIMIT, &query->limit) ==
        BBRouterFieldInvalid) return "Invalid limit";
    result = bb_router_body_int(json, &arguments, "after", 0, INT64_MAX, &query->after);
    if (result == BBRouterFieldInvalid) return "Invalid after";
    query->hasAfter = result == BBRouterFieldOK;
    result = bb_router_body_int(json, &arguments, "before", 0, INT64_MAX, &query->before);
    if (result == BBRouterFieldInvalid) return "Invalid before";
    query->hasBefore = result == BBRouterFieldOK;
    result = bb_router_body_string(json, &arguments, "sort", text, sizeof(text));
    if (result == BBRouterFieldInvalid ||
        (result == BBRouterFieldOK && !bb_router_apply_sort(query, text,
                                                            bb_router_string_length(text))))
        return "Invalid sort";
    if (bb_router_body_bool(json, &arguments, "withLastMessage", &query->withLastMessage) ==
        BBRouterFieldInvalid) return "Invalid withLastMessage";
    if (bb_router_body_bool(json, &arguments, "withArchived", &query->withArchived) ==
        BBRouterFieldInvalid) return "Invalid withArchived";
    if (bb_router_body_bool(json, &arguments, "withAttachments", &query->withAttachments) ==
        BBRouterFieldInvalid) return "Invalid withAttachments";
    if (bb_router_body_bool(json, &arguments, "withHandle", &query->withHandle) ==
        BBRouterFieldInvalid) return "Invalid withHandle";
    if (bb_router_body_bool(json, &arguments, "withChats", &query->withChats) ==
        BBRouterFieldInvalid) return "Invalid withChats";
    if (bb_router_body_bool(json, &arguments, "withChatParticipants",
                            &query->withChatParticipants) == BBRouterFieldInvalid)
        return "Invalid withChatParticipants";
    if (query->withChatParticipants) query->withChats = true;

    /* Identifiers: chatGuid, identifier, or guid, whichever is present. */
    result = bb_router_body_string(json, &arguments, "chatGuid", query->guid, sizeof(query->guid));
    if (result == BBRouterFieldAbsent)
        result = bb_router_body_string(json, &arguments, "identifier", query->guid,
                                       sizeof(query->guid));
    if (result == BBRouterFieldAbsent)
        result = bb_router_body_string(json, &arguments, "guid", query->guid,
                                       sizeof(query->guid));
    if (result == BBRouterFieldInvalid) return "Invalid identifier";
    query->hasGuid = result == BBRouterFieldOK && query->guid[0] != '\0';
    return NULL;
}

bool bb_router_socket_event(const BBRouterConfig *config,
                            BBEIOSession *session,
                            const unsigned char *packet,
                            size_t packetLength,
                            const BBSIOPacket *parsed,
                            char *scratch,
                            size_t scratchCapacity)
{
    size_t length = 0;
    BBRouterQuery query;
    bool hasArguments = false;
    const char *problem;
    BBRouterOperation operation;
    char arguments[BB_REQUEST_MAX_ARGUMENTS_BYTES];
    size_t argumentsLength = 0;
    BBRequest *request;

    if (!config || !session || !packet || !parsed || !scratch ||
        scratchCapacity < 256 || parsed->socketType != BBSIOPacketEvent) return false;
    /* Without an ACK id the reference answers on an operation channel the
     * stock client never listens to; dropping is the truthful equivalent. */
    if (!parsed->hasAckID) return true;

    if (bb_sio_event_name_equals(packet, packetLength, parsed, "get-server-metadata")) {
        if (!bb_router_write_metadata_envelope(config, "Successfully fetched metadata",
                                               scratch, scratchCapacity, &length))
            return false;
        return bb_eio_session_ack(session, parsed->ackID, scratch, length);
    }

    if (bb_sio_event_name_equals(packet, packetLength, parsed, "start-chat")) {
        /* Write event: its arguments differ from the read events. */
        operation = BBRouterOperationChatNew;
        problem = bb_router_new_chat_from_event(packet, parsed, &query);
        if (problem) {
            return bb_router_socket_ack_error(session, parsed->ackID, scratch, scratchCapacity,
                                              400, "Bad Request", "Validation Error", problem);
        }
    } else {
        if (bb_sio_event_name_equals(packet, packetLength, parsed, "get-chats"))
            operation = BBRouterOperationChatQuery;
        else if (bb_sio_event_name_equals(packet, packetLength, parsed, "get-chat"))
            operation = BBRouterOperationChatGet;
        else if (bb_sio_event_name_equals(packet, packetLength, parsed, "get-chat-messages"))
            operation = BBRouterOperationChatMessages;
        else if (bb_sio_event_name_equals(packet, packetLength, parsed, "get-messages"))
            operation = BBRouterOperationMessageQuery;
        else if (bb_sio_event_name_equals(packet, packetLength, parsed, "get-attachment"))
            operation = BBRouterOperationAttachmentGet;
        else {
            return bb_router_socket_ack_error(session, parsed->ackID, scratch, scratchCapacity,
                                              400, "Bad Request", "Validation Error",
                                              "Unsupported event");
        }

        problem = bb_router_query_from_event(packet, parsed, &query, &hasArguments);
        if (problem) {
            return bb_router_socket_ack_error(session, parsed->ackID, scratch, scratchCapacity,
                                              400, "Bad Request", "Validation Error", problem);
        }
        if ((operation == BBRouterOperationChatGet || operation == BBRouterOperationChatMessages ||
             operation == BBRouterOperationAttachmentGet) && !query.hasGuid) {
            return bb_router_socket_ack_error(session, parsed->ackID, scratch, scratchCapacity,
                                              400, "Bad Request", "Validation Error",
                                              "Missing identifier");
        }
    }
    if (!config->requests) {
        return bb_router_socket_ack_error(session, parsed->ackID, scratch, scratchCapacity,
                                          503, "Service Unavailable", "Server Error",
                                          "The messaging bridge is unavailable");
    }
    if (!bb_router_write_arguments(operation, &query, arguments, sizeof(arguments),
                                   &argumentsLength)) {
        return bb_router_socket_ack_error(session, parsed->ackID, scratch, scratchCapacity,
                                          400, "Bad Request", "Validation Error",
                                          "Request arguments are too large");
    }
    request = bb_request_table_open_ack(config->requests, session, parsed->ackID,
                                        bb_router_operation_name(operation), arguments,
                                        argumentsLength, bb_router_now(config));
    if (!request) {
        return bb_router_socket_ack_error(session, parsed->ackID, scratch, scratchCapacity,
                                          503, "Service Unavailable", "Server Error",
                                          "Too many pending requests");
    }
    /* The session is already live, so the request can leave immediately. */
    (void)bb_request_table_dispatch(config->requests, request);
    return true;
}

/* ---- entry point ----------------------------------------------------- */

bool bb_router_route(const BBRouterConfig *config,
                     BBConnection *connection,
                     const BBHTTPRequest *request,
                     const unsigned char *body,
                     size_t bodyLength,
                     char *scratch,
                     size_t scratchCapacity,
                     BBHTTPResponse *response)
{
    BBRouteContext context;
    if (!config || !request || !scratch || scratchCapacity < 256 || !response)
        return false;
    bb_response_init(response);

    /* CORS preflight never authenticates and carries no body. */
    if (request->method == BBHTTPMethodOPTIONS) {
        response->status = 204;
        return true;
    }

    if (!config->password || !config->password[0]) {
        return bb_router_error(response, scratch, scratchCapacity, 500,
                               "Server Error", "Server Error",
                               "Server password is not configured");
    }
    switch (bb_router_check_credentials(request->query, config->password)) {
        case BBRouterAuthOK:
            break;
        case BBRouterAuthMissing:
            return bb_router_error(response, scratch, scratchCapacity, 401,
                                   "You are not authorized to access this resource",
                                   "Authentication Error",
                                   "Missing server password!");
        default:
            return bb_router_error(response, scratch, scratchCapacity, 401,
                                   "You are not authorized to access this resource",
                                   "Authentication Error", "Unauthorized");
    }

    if (bb_router_path_equals(request->path, "/socket.io")) {
        return bb_router_socket_io(config, request, body, bodyLength, scratch,
                                   scratchCapacity, response);
    }

    if (!bb_router_path_has_prefix(request->path, "/api/v1/")) {
        return bb_router_error(response, scratch, scratchCapacity, 404,
                               "Not Found", "Validation Error",
                               "Route not found");
    }

    context.config = config;
    context.request = request;
    context.connection = connection;
    context.body = body;
    context.bodyLength = bodyLength;
    context.scratch = scratch;
    context.scratchCapacity = scratchCapacity;
    context.response = response;
    return bb_router_api(&context);
}

/* ---- Attachment upload (streamed multipart) -------------------------- */

enum {
    BBRouterUploadFieldNone = 0,
    BBRouterUploadFieldIgnored,
    BBRouterUploadFieldChatGuid,
    BBRouterUploadFieldTempGuid,
    BBRouterUploadFieldName,
    BBRouterUploadFieldEffectId,
    BBRouterUploadFieldSubject,
    BBRouterUploadFieldReply,
    BBRouterUploadFieldFile
};

static void bb_router_upload_reject(BBRouterUpload *upload, int status,
                                    const char *message, const char *type,
                                    const char *detail)
{
    if (upload->rejected) return;   /* the first problem wins */
    upload->rejected = true;
    upload->rejectStatus = status;
    upload->rejectMessage = message;
    upload->rejectType = type;
    upload->rejectDetail = detail;
}

static void bb_router_upload_validation(BBRouterUpload *upload, const char *detail)
{
    bb_router_upload_reject(upload, 400, "Bad Request", "Validation Error", detail);
}

static void bb_router_upload_close_file(BBRouterUpload *upload, bool keep)
{
    if (!upload->fileOpen) return;
    upload->fileOpen = false;
    upload->config->uploads.closeFile(upload->config->uploads.context, upload, keep);
}

/* Which buffer a text field fills. */
static char *bb_router_upload_field_buffer(BBRouterUpload *upload, size_t *capacity)
{
    switch (upload->field) {
        case BBRouterUploadFieldChatGuid: *capacity = sizeof(upload->chatGuid); return upload->chatGuid;
        case BBRouterUploadFieldTempGuid: *capacity = sizeof(upload->tempGuid); return upload->tempGuid;
        case BBRouterUploadFieldName: *capacity = sizeof(upload->name); return upload->name;
        default: *capacity = 0; return NULL;
    }
}

static bool bb_router_upload_part_begin(void *context, const BBMultipartPart *part)
{
    BBRouterUpload *upload = (BBRouterUpload *)context;
    const char *name = part->name;
    size_t nameLength = bb_router_string_length(name);
    upload->fieldLength = 0;
    if (part->hasFilename || bb_router_ascii_equal_case(name, nameLength, "attachment")) {
        if (upload->fileSeen) {
            bb_router_upload_validation(upload, "Only one attachment per request");
            upload->field = BBRouterUploadFieldIgnored;
            return true;
        }
        upload->fileSeen = true;
        upload->field = BBRouterUploadFieldFile;
        upload->fileBytes = 0;
        bb_router_copy_string(upload->fileContentType, sizeof(upload->fileContentType),
                              part->contentType);
        if (upload->rejected) return true;   /* draining: no file */
        if (!upload->config->uploads.openFile(upload->config->uploads.context, upload,
                                              part->filename, part->contentType) ||
            !upload->uploadPath[0]) {
            bb_router_upload_reject(upload, 500, "Server Error", "Server Error",
                                    "Could not stage the attachment");
            return true;
        }
        upload->fileOpen = true;
        return true;
    }
    if (bb_router_ascii_equal_case(name, nameLength, "chatGuid")) upload->field = BBRouterUploadFieldChatGuid;
    else if (bb_router_ascii_equal_case(name, nameLength, "tempGuid")) upload->field = BBRouterUploadFieldTempGuid;
    else if (bb_router_ascii_equal_case(name, nameLength, "name")) upload->field = BBRouterUploadFieldName;
    else if (bb_router_ascii_equal_case(name, nameLength, "effectId")) upload->field = BBRouterUploadFieldEffectId;
    else if (bb_router_ascii_equal_case(name, nameLength, "subject")) upload->field = BBRouterUploadFieldSubject;
    else if (bb_router_ascii_equal_case(name, nameLength, "selectedMessageGuid")) upload->field = BBRouterUploadFieldReply;
    else upload->field = BBRouterUploadFieldIgnored;   /* method, partIndex, isAudioMessage, ... */
    {
        size_t capacity = 0;
        char *buffer = bb_router_upload_field_buffer(upload, &capacity);
        if (buffer) buffer[0] = '\0';
    }
    return true;
}

static bool bb_router_upload_part_data(void *context, const unsigned char *bytes, size_t length)
{
    BBRouterUpload *upload = (BBRouterUpload *)context;
    size_t capacity = 0;
    char *buffer;
    switch (upload->field) {
        case BBRouterUploadFieldFile:
            upload->fileBytes += length;
            if (upload->fileBytes > BB_ROUTER_MAX_UPLOAD_BYTES) {
                bb_router_upload_close_file(upload, false);
                bb_router_upload_reject(upload, 413, "Payload Too Large", "Validation Error",
                                        "Attachment exceeds the upload limit");
                return true;
            }
            if (!upload->fileOpen) return true;   /* draining */
            if (!upload->config->uploads.writeFile(upload->config->uploads.context, upload,
                                                   bytes, length)) {
                bb_router_upload_close_file(upload, false);
                bb_router_upload_reject(upload, 500, "Server Error", "Server Error",
                                        "Could not write the attachment");
            }
            return true;
        case BBRouterUploadFieldEffectId:
        case BBRouterUploadFieldSubject:
        case BBRouterUploadFieldReply:
            /* Any content at all means the client asked for it. */
            if (length) {
                bb_router_upload_validation(upload,
                    upload->field == BBRouterUploadFieldEffectId ? "Message effects are not supported on iOS 9" :
                    upload->field == BBRouterUploadFieldSubject ? "Message subjects are not supported on iOS 9" :
                    "Replies are not supported on iOS 9");
            }
            return true;
        default:
            break;
    }
    buffer = bb_router_upload_field_buffer(upload, &capacity);
    if (!buffer) return true;   /* ignored field */
    if (upload->fieldLength + length + 1 > capacity) {
        bb_router_upload_validation(upload, "A form field is too long");
        upload->field = BBRouterUploadFieldIgnored;
        return true;
    }
    for (size_t i = 0; i < length; i++) {
        if (!bytes[i]) {
            bb_router_upload_validation(upload, "A form field contains NUL");
            upload->field = BBRouterUploadFieldIgnored;
            return true;
        }
        buffer[upload->fieldLength + i] = (char)bytes[i];
    }
    upload->fieldLength += length;
    buffer[upload->fieldLength] = '\0';
    return true;
}

static bool bb_router_upload_part_end(void *context)
{
    BBRouterUpload *upload = (BBRouterUpload *)context;
    /* The file stays open until the whole body is validated, so a late
     * problem can still discard it. */
    upload->field = BBRouterUploadFieldNone;
    return true;
}

bool bb_router_upload_begin(const BBRouterConfig *config,
                            BBConnection *connection,
                            const BBHTTPRequest *request,
                            BBRouterUpload *upload)
{
    BBMultipartCallbacks callbacks;
    if (!config || !request || !upload) return false;
    if (request->method != BBHTTPMethodPOST ||
        !bb_router_path_equals(request->path, "/api/v1/message/attachment")) return false;

    bb_router_wipe((char *)upload, sizeof(*upload));
    upload->config = config;
    upload->connection = connection;
    upload->multipart.state = BBMultipartStateFailed;

    /* Claimed from here on: problems are answered after draining. */
    if (!config->password || !config->password[0]) {
        bb_router_upload_reject(upload, 500, "Server Error", "Server Error",
                                "Server password is not configured");
    } else {
        switch (bb_router_check_credentials(request->query, config->password)) {
            case BBRouterAuthOK: break;
            case BBRouterAuthMissing:
                bb_router_upload_reject(upload, 401, "You are not authorized to access this resource",
                                        "Authentication Error", "Missing server password!");
                break;
            default:
                bb_router_upload_reject(upload, 401, "You are not authorized to access this resource",
                                        "Authentication Error", "Unauthorized");
                break;
        }
    }
    if (!config->uploads.openFile || !config->uploads.writeFile || !config->uploads.closeFile) {
        bb_router_upload_reject(upload, 501, "Not Implemented", "Server Error",
                                "Attachment uploads are unavailable");
    } else if (!config->requests || !connection) {
        bb_router_upload_reject(upload, 503, "Service Unavailable", "Server Error",
                                "The messaging bridge is unavailable");
    }
    if (request->bodyLength > BB_ROUTER_MAX_UPLOAD_BYTES + BB_MULTIPART_MAX_PART_HEAD_BYTES * BB_MULTIPART_MAX_PARTS) {
        bb_router_upload_reject(upload, 413, "Payload Too Large", "Validation Error",
                                "Attachment exceeds the upload limit");
    }
    callbacks.partBegin = bb_router_upload_part_begin;
    callbacks.partData = bb_router_upload_part_data;
    callbacks.partEnd = bb_router_upload_part_end;
    callbacks.context = upload;
    if (!bb_multipart_init(&upload->multipart, request->contentType, &callbacks)) {
        bb_router_upload_validation(upload, "Expected a multipart/form-data body");
    }
    return true;
}

bool bb_router_upload_data(BBRouterUpload *upload, const unsigned char *bytes, size_t length)
{
    if (!upload || !upload->config) return false;
    if (upload->multipart.state == BBMultipartStateFailed) {
        /* No parser (bad content type): drain to answer the recorded error. */
        return upload->rejected;
    }
    if (!bb_multipart_feed(&upload->multipart, bytes, length)) {
        bb_router_upload_close_file(upload, false);
        return false;   /* malformed multipart: refuse outright */
    }
    return true;
}

static bool bb_router_upload_write_arguments(const BBRouterUpload *upload,
                                             char *out, size_t capacity, size_t *outLength)
{
    BBJSONWriter writer;
    bb_json_writer_init(&writer, out, capacity);
    return bb_json_write_raw(&writer, "{") &&
           bb_json_write_key(&writer, "chatGuid") &&
           bb_json_write_string(&writer, upload->chatGuid) &&
           bb_json_write_raw(&writer, ",") &&
           bb_json_write_key(&writer, "tempGuid") &&
           bb_json_write_string(&writer, upload->tempGuid) &&
           bb_json_write_raw(&writer, ",") &&
           bb_json_write_key(&writer, "name") &&
           bb_json_write_string(&writer, upload->name) &&
           bb_json_write_raw(&writer, ",") &&
           bb_json_write_key(&writer, "contentType") &&
           bb_json_write_string(&writer, upload->fileContentType[0] ? upload->fileContentType : NULL) &&
           bb_json_write_raw(&writer, ",") &&
           bb_json_write_key(&writer, "uploadPath") &&
           bb_json_write_string(&writer, upload->uploadPath) &&
           bb_json_write_raw(&writer, ",") &&
           bb_json_write_key(&writer, "bytes") &&
           bb_json_write_signed(&writer, (int64_t)upload->fileBytes) &&
           bb_json_write_raw(&writer, "}") &&
           bb_json_writer_finish(&writer, outLength);
}

bool bb_router_upload_end(BBRouterUpload *upload,
                          char *scratch, size_t scratchCapacity,
                          BBHTTPResponse *response)
{
    char arguments[BB_REQUEST_MAX_ARGUMENTS_BYTES];
    size_t argumentsLength = 0;
    BBRequest *request;
    if (!upload || !upload->config || !scratch || scratchCapacity < 256 || !response) return false;
    bb_response_init(response);
    if (!upload->rejected) {
        if (!bb_multipart_finish(&upload->multipart))
            bb_router_upload_validation(upload, "Malformed multipart body");
        else if (!upload->fileSeen)
            bb_router_upload_validation(upload, "Multipart field attachment is required");
        else if (!upload->chatGuid[0])
            bb_router_upload_validation(upload, "Missing chatGuid");
        else if (!upload->tempGuid[0])
            bb_router_upload_validation(upload, "Missing tempGuid");
        else if (!upload->fileBytes)
            bb_router_upload_validation(upload, "Attachment is empty");
    }
    if (upload->rejected) {
        bb_router_upload_close_file(upload, false);
        return bb_router_error(response, scratch, scratchCapacity, upload->rejectStatus,
                               upload->rejectMessage, upload->rejectType, upload->rejectDetail);
    }
    if (!upload->name[0]) {
        /* The client always sends name; fall back to a fixed one. */
        bb_router_copy_string(upload->name, sizeof(upload->name), "attachment");
    }
    if (!bb_router_upload_write_arguments(upload, arguments, sizeof(arguments), &argumentsLength)) {
        bb_router_upload_close_file(upload, false);
        return bb_router_error(response, scratch, scratchCapacity, 400, "Bad Request",
                               "Validation Error", "Request arguments are too large");
    }
    request = bb_request_table_open_http(upload->config->requests, upload->connection,
                                         "message.attachment", arguments, argumentsLength,
                                         bb_router_now(upload->config));
    if (!request) {
        bb_router_upload_close_file(upload, false);
        return bb_router_error(response, scratch, scratchCapacity, 503, "Service Unavailable",
                               "Server Error", "Too many pending requests");
    }
    /* Closed (and kept) before the pending callback dispatches it. */
    bb_router_upload_close_file(upload, true);
    response->pending = true;
    response->handlerContext = request;
    return true;
}

void bb_router_upload_abort(BBRouterUpload *upload)
{
    if (!upload || !upload->config) return;
    bb_router_upload_close_file(upload, false);
    upload->multipart.state = BBMultipartStateFailed;
}
