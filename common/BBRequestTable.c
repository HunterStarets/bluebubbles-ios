#include "BBRequestTable.h"

#include "BBJSON.h"
#include "BBResponse.h"

/* Volatile loops keep the older armv7 Clang from lowering these into libc
 * calls the SpringBoard target must not import. */
static void bb_request_memory_set(void *destination, unsigned char value,
                                  size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static void bb_request_memory_copy(void *destination, const void *source,
                                   size_t length)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;
    for (size_t i = 0; i < length; i++) out[i] = in[i];
}

static size_t bb_request_string_length(const char *value)
{
    const volatile char *bytes = (const volatile char *)value;
    size_t length = 0;
    if (!value) return 0;
    while (bytes[length]) length++;
    return length;
}

bool bb_request_table_init(BBRequestTable *table,
                           BBRequest *requests, size_t capacity,
                           uint64_t firstRequestID, uint64_t timeoutMs,
                           BBRequestDispatch dispatch, void *context,
                           char *scratch, size_t scratchCapacity)
{
    if (!table) return false;
    bb_request_memory_set(table, 0, sizeof(*table));
    if (!requests || !capacity || !dispatch || !scratch || scratchCapacity < 256)
        return false;
    bb_request_memory_set(requests, 0, sizeof(*requests) * capacity);
    table->requests = requests;
    table->capacity = capacity;
    table->nextRequestID = firstRequestID ? firstRequestID : 1;
    table->timeoutMs = timeoutMs ? timeoutMs : BB_REQUEST_DEFAULT_TIMEOUT_MS;
    table->dispatch = dispatch;
    table->context = context;
    table->scratch = scratch;
    table->scratchCapacity = scratchCapacity;
    return true;
}

static void bb_request_release(BBRequest *request)
{
    bb_request_memory_set(request, 0, sizeof(*request));
}

static BBRequest *bb_request_table_open(BBRequestTable *table,
                                        const char *operation,
                                        const char *argumentsJSON,
                                        size_t argumentsLength,
                                        uint64_t nowMs)
{
    BBJSONValue arguments;
    size_t operationLength = bb_request_string_length(operation);
    if (!table || !table->requests || !operationLength ||
        operationLength >= BB_REQUEST_MAX_OPERATION_BYTES) return NULL;
    if (!argumentsJSON || !argumentsLength ||
        argumentsLength >= BB_REQUEST_MAX_ARGUMENTS_BYTES ||
        !bb_json_parse(argumentsJSON, argumentsLength, &arguments) ||
        arguments.type != BBJSONTypeObject) return NULL;
    for (size_t i = 0; i < table->capacity; i++) {
        BBRequest *request = &table->requests[i];
        if (request->used) continue;
        bb_request_release(request);
        request->used = true;
        request->requestID = table->nextRequestID++;
        if (table->nextRequestID == 0) table->nextRequestID = 1;
        request->openedAt = nowMs;
        bb_request_memory_copy(request->operation, operation, operationLength + 1);
        bb_request_memory_copy(request->argumentsJSON, argumentsJSON, argumentsLength);
        request->argumentsJSON[argumentsLength] = '\0';
        request->argumentsLength = argumentsLength;
        table->opened++;
        return request;
    }
    table->refused++;
    return NULL;
}

BBRequest *bb_request_table_open_http(BBRequestTable *table,
                                      BBConnection *connection,
                                      const char *operation,
                                      const char *argumentsJSON,
                                      size_t argumentsLength,
                                      uint64_t nowMs)
{
    BBRequest *request;
    if (!connection) return NULL;
    request = bb_request_table_open(table, operation, argumentsJSON,
                                    argumentsLength, nowMs);
    if (!request) return NULL;
    request->origin = BBRequestOriginHTTP;
    request->connection = connection;
    return request;
}

BBRequest *bb_request_table_open_ack(BBRequestTable *table,
                                     BBEIOSession *session,
                                     uint64_t ackID,
                                     const char *operation,
                                     const char *argumentsJSON,
                                     size_t argumentsLength,
                                     uint64_t nowMs)
{
    BBRequest *request;
    if (!session) return NULL;
    request = bb_request_table_open(table, operation, argumentsJSON,
                                    argumentsLength, nowMs);
    if (!request) return NULL;
    request->origin = BBRequestOriginSocketAck;
    request->session = session;
    request->ackID = ackID;
    return request;
}

/* Deliver an envelope to the request's origin and free the slot. */
static void bb_request_finish(BBRequestTable *table, BBRequest *request,
                              int status, const char *message,
                              const char *dataJSON, size_t dataLength,
                              const char *metadataJSON, size_t metadataLength,
                              const char *errorJSON, size_t errorLength)
{
    if (request->origin == BBRequestOriginHTTP) {
        BBConnection *connection = request->connection;
        BBHTTPResponse response;
        size_t length = 0;
        bb_response_init(&response);
        response.contentType = BB_RESPONSE_CONTENT_TYPE_JSON;
        if (bb_response_envelope(connection->scratch, connection->scratchCapacity,
                                 &length, status, message, dataJSON, dataLength,
                                 metadataJSON, metadataLength, errorJSON,
                                 errorLength)) {
            response.status = status;
        } else if (bb_response_error_envelope(connection->scratch,
                                              connection->scratchCapacity, &length,
                                              500, "Server Error", "Server Error",
                                              "Reply exceeds the response buffer")) {
            response.status = 500;
        } else {
            response.status = 500;
            response.contentType = NULL;
            length = 0;
        }
        response.body = (const unsigned char *)connection->scratch;
        response.bodyLength = length;
        (void)bb_connection_complete(connection, &response);
    } else if (request->origin == BBRequestOriginSocketAck) {
        size_t length = 0;
        if (bb_eio_session_is_live(request->session) &&
            bb_response_envelope(table->scratch, table->scratchCapacity, &length,
                                 status, message, dataJSON, dataLength,
                                 metadataJSON, metadataLength, errorJSON,
                                 errorLength)) {
            (void)bb_eio_session_ack(request->session, request->ackID,
                                     table->scratch, length);
        } else if (bb_eio_session_is_live(request->session) &&
                   bb_response_error_envelope(table->scratch, table->scratchCapacity,
                                              &length, 500, "Server Error",
                                              "Server Error",
                                              "Reply exceeds the response buffer")) {
            (void)bb_eio_session_ack(request->session, request->ackID,
                                     table->scratch, length);
        }
    }
    bb_request_release(request);
}

/* Finish with a fixed error object; the JSON text is a literal. */
static void bb_request_finish_error(BBRequestTable *table, BBRequest *request,
                                    int status, const char *message,
                                    const char *errorJSON)
{
    bb_request_finish(table, request, status, message, NULL, 0, NULL, 0,
                      errorJSON, bb_request_string_length(errorJSON));
}

bool bb_request_table_dispatch(BBRequestTable *table, BBRequest *request)
{
    if (!table || !request || !request->used || request->dispatched) return false;
    request->dispatched = true;
    if (table->dispatch(table->context, table, request)) return true;
    table->refused++;
    bb_request_finish_error(table, request, 503, "Service Unavailable",
                            "{\"type\":\"Server Error\",\"message\":"
                            "\"The messaging bridge is unavailable\"}");
    return false;
}

static bool bb_request_valid_json(const char *json, size_t length)
{
    BBJSONValue value;
    if (!length) return true;
    return json && bb_json_parse(json, length, &value);
}

bool bb_request_table_complete(BBRequestTable *table,
                               uint64_t requestID,
                               int status,
                               const char *message,
                               const char *dataJSON, size_t dataLength,
                               const char *metadataJSON, size_t metadataLength,
                               const char *errorJSON, size_t errorLength)
{
    BBRequest *request = bb_request_table_find(table, requestID);
    if (!request) return false;
    if (status < 100 || status > 599 ||
        !bb_request_valid_json(dataJSON, dataLength) ||
        !bb_request_valid_json(metadataJSON, metadataLength) ||
        !bb_request_valid_json(errorJSON, errorLength)) {
        /* A malformed bridge reply must not reach the client as-is. */
        bb_request_finish_error(table, request, 500, "Server Error",
                                "{\"type\":\"Server Error\",\"message\":"
                                "\"Malformed bridge reply\"}");
        table->completed++;
        return true;
    }
    bb_request_finish(table, request, status, message, dataJSON, dataLength,
                      metadataJSON, metadataLength, errorJSON, errorLength);
    table->completed++;
    return true;
}

bool bb_request_table_complete_raw(BBRequestTable *table,
                                   uint64_t requestID,
                                   int status,
                                   const char *contentType,
                                   const unsigned char *body,
                                   size_t bodyLength)
{
    BBRequest *request = bb_request_table_find(table, requestID);
    BBConnection *connection;
    BBHTTPResponse response;
    if (!request) return false;
    if (request->origin != BBRequestOriginHTTP || status < 100 || status > 599 ||
        (bodyLength && !body)) {
        bb_request_release(request);
        return false;
    }
    connection = request->connection;
    bb_response_init(&response);
    response.status = status;
    response.contentType = contentType;
    response.body = body;
    response.bodyLength = bodyLength;
    (void)bb_connection_complete(connection, &response);
    bb_request_release(request);
    table->completed++;
    return true;
}

size_t bb_request_table_expire(BBRequestTable *table, uint64_t nowMs)
{
    size_t count = 0;
    if (!table || !table->requests) return 0;
    for (size_t i = 0; i < table->capacity; i++) {
        BBRequest *request = &table->requests[i];
        if (!request->used || nowMs < request->openedAt ||
            nowMs - request->openedAt < table->timeoutMs) continue;
        bb_request_finish_error(table, request, 504, "Gateway Timeout",
                                "{\"type\":\"Gateway Timeout\",\"message\":"
                                "\"The messaging bridge did not reply in time\"}");
        table->expired++;
        count++;
    }
    return count;
}

size_t bb_request_table_cancel_connection(BBRequestTable *table,
                                          const BBConnection *connection)
{
    size_t count = 0;
    if (!table || !table->requests || !connection) return 0;
    for (size_t i = 0; i < table->capacity; i++) {
        BBRequest *request = &table->requests[i];
        if (request->used && request->origin == BBRequestOriginHTTP &&
            request->connection == connection) {
            bb_request_release(request);
            table->cancelled++;
            count++;
        }
    }
    return count;
}

size_t bb_request_table_cancel_session(BBRequestTable *table,
                                       const BBEIOSession *session)
{
    size_t count = 0;
    if (!table || !table->requests || !session) return 0;
    for (size_t i = 0; i < table->capacity; i++) {
        BBRequest *request = &table->requests[i];
        if (request->used && request->origin == BBRequestOriginSocketAck &&
            request->session == session) {
            bb_request_release(request);
            table->cancelled++;
            count++;
        }
    }
    return count;
}

BBRequest *bb_request_table_find(const BBRequestTable *table, uint64_t requestID)
{
    if (!table || !table->requests) return NULL;
    for (size_t i = 0; i < table->capacity; i++) {
        BBRequest *request = &table->requests[i];
        if (request->used && request->requestID == requestID) return request;
    }
    return NULL;
}

size_t bb_request_table_active_count(const BBRequestTable *table)
{
    size_t count = 0;
    if (!table || !table->requests) return 0;
    for (size_t i = 0; i < table->capacity; i++) {
        if (table->requests[i].used) count++;
    }
    return count;
}
