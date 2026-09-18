#include "BBConnection.h"

/* Volatile loops keep the older armv7 Clang from lowering these into the
 * libc calls the SpringBoard target must not import. */
static void bb_connection_memory_set(void *destination, unsigned char value,
                                     size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static void bb_connection_memory_copy(void *destination, const void *source,
                                      size_t length)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;
    for (size_t i = 0; i < length; i++) out[i] = in[i];
}

bool bb_connection_init(BBConnection *connection,
                        unsigned char *input,
                        size_t inputCapacity,
                        char *scratch,
                        size_t scratchCapacity,
                        const BBConnectionCallbacks *callbacks)
{
    if (!connection) return false;
    bb_connection_memory_set(connection, 0, sizeof(*connection));
    if (!input || inputCapacity == 0 || !scratch || scratchCapacity < 256 ||
        !callbacks || !callbacks->handleRequest || !callbacks->emit ||
        (callbacks->streamBegin && (!callbacks->streamData || !callbacks->streamEnd))) {
        connection->state = BBConnectionStateClosing;
        return false;
    }
    connection->input = input;
    connection->inputCapacity = inputCapacity;
    connection->scratch = scratch;
    connection->scratchCapacity = scratchCapacity;
    connection->callbacks = *callbacks;
    connection->state = BBConnectionStateHTTP;
    return true;
}

bool bb_connection_wants_close(const BBConnection *connection)
{
    return !connection || connection->state == BBConnectionStateClosing;
}

static void bb_connection_stream_abort(BBConnection *connection)
{
    void *streamContext = connection->streamContext;
    connection->streamContext = NULL;
    connection->streamRemaining = 0;
    if (streamContext && connection->callbacks.streamAbort) {
        connection->callbacks.streamAbort(connection->callbacks.context,
                                          connection, streamContext);
    }
}

void bb_connection_abort(BBConnection *connection)
{
    if (!connection) return;
    if (connection->state == BBConnectionStateStreaming) bb_connection_stream_abort(connection);
    connection->state = BBConnectionStateClosing;
    connection->inputLength = 0;
}

BBConnectionState bb_connection_state(const BBConnection *connection)
{
    return connection ? connection->state : BBConnectionStateClosing;
}

static bool bb_connection_emit(BBConnection *connection,
                               const unsigned char *bytes, size_t length)
{
    if (!length) return true;
    if (!connection->callbacks.emit(connection->callbacks.context, connection,
                                    bytes, length)) {
        connection->state = BBConnectionStateClosing;
        return false;
    }
    return true;
}

/* Serialize and emit one response. keepAlive already accounts for the
 * request's framing; the response may still force a close. */
static bool bb_connection_send_response(BBConnection *connection,
                                        const BBHTTPResponse *response,
                                        bool keepAlive)
{
    size_t headLength = 0;
    if (!bb_response_write_head(response, keepAlive, connection->head,
                                sizeof(connection->head), &headLength)) {
        connection->state = BBConnectionStateClosing;
        return false;
    }
    if (!bb_connection_emit(connection, (const unsigned char *)connection->head,
                            headLength)) return false;
    if (response->status != 101 &&
        !bb_connection_emit(connection, response->body, response->bodyLength))
        return false;
    connection->responsesEmitted++;
    return true;
}

static void bb_connection_reject(BBConnection *connection, int status,
                                 const char *detail)
{
    BBHTTPResponse response;
    size_t length = 0;
    connection->requestsRejected++;
    if (connection->state == BBConnectionStateStreaming) bb_connection_stream_abort(connection);
    bb_response_init(&response);
    response.closeConnection = true;
    if (bb_response_error_envelope(connection->scratch,
                                   connection->scratchCapacity, &length, status,
                                   NULL, "Validation Error", detail)) {
        response.status = status;
        response.contentType = BB_RESPONSE_CONTENT_TYPE_JSON;
        response.body = (const unsigned char *)connection->scratch;
        response.bodyLength = length;
    } else {
        response.status = status;
    }
    (void)bb_connection_send_response(connection, &response, false);
    connection->state = BBConnectionStateClosing;
    connection->inputLength = 0;
}

static void bb_connection_consume(BBConnection *connection, size_t length)
{
    size_t remaining;
    if (length >= connection->inputLength) {
        connection->inputLength = 0;
        return;
    }
    remaining = connection->inputLength - length;
    bb_connection_memory_copy(connection->input, connection->input + length,
                              remaining);
    connection->inputLength = remaining;
}

static void bb_connection_deliver_upgraded(BBConnection *connection,
                                           const unsigned char *bytes,
                                           size_t length)
{
    if (!length) return;
    if (!connection->callbacks.upgradedData) {
        connection->state = BBConnectionStateClosing;
        return;
    }
    connection->upgradedBytes += length;
    connection->callbacks.upgradedData(connection->callbacks.context,
                                       connection, bytes, length);
}

/* Emit the response for the parsed request at the front of the input
 * buffer, consume it, and move to the next state. Returns true when more
 * pipelined input may be parsed. */
static bool bb_connection_finish(BBConnection *connection,
                                 const BBHTTPResponse *response)
{
    BBHTTPRequest *request = &connection->request;
    size_t messageLength = request->messageLength;
    bool keepAlive;

    if (response->pending ||
        (response->status == 101 && !request->upgradeWebSocket)) {
        bb_connection_reject(connection, 500, "Request handler failed");
        return false;
    }
    keepAlive = request->keepAlive && !response->closeConnection;
    connection->state = BBConnectionStateHTTP;
    if (!bb_connection_send_response(connection, response, keepAlive))
        return false;
    connection->requestsServed++;

    /* The response body may borrow from the input buffer, so compaction
     * happens only after the bytes have been emitted. */
    bb_connection_consume(connection, messageLength);

    if (response->status == 101) {
        connection->state = BBConnectionStateUpgraded;
        if (connection->callbacks.upgraded) {
            connection->callbacks.upgraded(connection->callbacks.context,
                                           connection,
                                           response->handlerContext);
        }
        if (connection->state == BBConnectionStateUpgraded) {
            bb_connection_deliver_upgraded(connection, connection->input,
                                           connection->inputLength);
        }
        connection->inputLength = 0;
        return false;
    }
    if (!keepAlive) {
        connection->state = BBConnectionStateClosing;
        connection->inputLength = 0;
        return false;
    }
    return connection->inputLength != 0;
}

/* Deliver the streamed body's final response. The request's body fields
 * are cleared: nothing of it is in the input buffer. */
static bool bb_connection_stream_end(BBConnection *connection)
{
    BBHTTPResponse response;
    void *streamContext = connection->streamContext;
    connection->request.messageLength = 0;
    connection->request.bodyLength = 0;
    connection->request.decodedBodyLength = 0;
    connection->request.bodyOffset = 0;
    connection->pendingBodyLength = 0;
    connection->streamContext = NULL;
    connection->streamRemaining = 0;
    connection->state = BBConnectionStateHTTP;
    bb_response_init(&response);
    if (!connection->callbacks.streamEnd(connection->callbacks.context, connection,
                                         streamContext, connection->scratch,
                                         connection->scratchCapacity, &response)) {
        if (connection->callbacks.streamAbort) {
            connection->callbacks.streamAbort(connection->callbacks.context,
                                              connection, streamContext);
        }
        bb_connection_reject(connection, 500, "Request handler failed");
        return false;
    }
    if (response.pending) {
        connection->state = BBConnectionStatePending;
        connection->pendingResponses++;
        if (connection->callbacks.pending) {
            connection->callbacks.pending(connection->callbacks.context,
                                          connection, response.handlerContext);
        }
        return false;
    }
    return bb_connection_finish(connection, &response);
}

/* Hand body bytes to the stream. Returns false once the upload was
 * refused (the connection is then closing). */
static bool bb_connection_stream_deliver(BBConnection *connection,
                                         const unsigned char *bytes, size_t length)
{
    if (length && !connection->callbacks.streamData(connection->callbacks.context,
                                                    connection,
                                                    connection->streamContext,
                                                    bytes, length)) {
        bb_connection_reject(connection, 400, "Upload refused");
        return false;
    }
    connection->streamRemaining -= length;
    return true;
}

/* The owner claimed the body of the request at the front of the buffer:
 * hand over what is already buffered, consume the head, and finish the
 * request if the whole body was already there. */
static bool bb_connection_stream_start(BBConnection *connection)
{
    BBHTTPRequest *request = &connection->request;
    size_t available = connection->inputLength - request->headerLength;
    size_t take = available < request->bodyLength ? available : request->bodyLength;
    connection->state = BBConnectionStateStreaming;
    connection->streamRemaining = request->bodyLength;
    /* Delivered in place, then consumed: the callbacks read the buffer. */
    if (!bb_connection_stream_deliver(connection, connection->input + request->headerLength,
                                      take)) return false;
    bb_connection_consume(connection, request->headerLength + take);
    if (connection->streamRemaining) return false;
    return bb_connection_stream_end(connection);
}

/* Handle exactly one complete request at the front of the input buffer.
 * Returns false when no further parsing should happen on this call. */
static bool bb_connection_service_one(BBConnection *connection)
{
    BBHTTPRequest *request = &connection->request;
    BBHTTPResponse response;
    const unsigned char *body;
    size_t bodyLength;

    BBHTTPParseResult result = bb_http_parse_request(connection->input,
                                                     connection->inputLength,
                                                     request);
    if (result == BBHTTPParseInvalid) {
        bb_connection_reject(connection, 400, "Malformed HTTP request");
        return false;
    }
    /* A Content-Length body, complete or not, may be claimed by the owner
     * and streamed instead of buffered. Offered as soon as the head is in. */
    if (request->headComplete && !request->chunked && request->bodyLength &&
        connection->callbacks.streamBegin &&
        connection->callbacks.streamBegin(connection->callbacks.context, connection,
                                          request, &connection->streamContext)) {
        return bb_connection_stream_start(connection);
    }
    if (result == BBHTTPParseNeedMore) {
        if (connection->inputLength >= connection->inputCapacity) {
            bb_connection_reject(connection, 413,
                                 "Request exceeds the connection buffer");
        }
        return false;
    }

    body = connection->input + request->bodyOffset;
    bodyLength = request->bodyLength;
    if (request->chunked) {
        if (!bb_http_decode_chunked_body_in_place(connection->input +
                                                  request->bodyOffset,
                                                  request->bodyLength,
                                                  &bodyLength)) {
            bb_connection_reject(connection, 400, "Malformed chunked body");
            return false;
        }
    }
    connection->pendingBodyLength = bodyLength;

    bb_response_init(&response);
    if (!connection->callbacks.handleRequest(connection->callbacks.context,
                                             connection, request, body,
                                             bodyLength, connection->scratch,
                                             connection->scratchCapacity,
                                             &response)) {
        bb_connection_reject(connection, 500, "Request handler failed");
        return false;
    }
    if (response.pending) {
        /* Keep the request and its body in place; later input is only
         * buffered until bb_connection_complete runs. */
        connection->state = BBConnectionStatePending;
        connection->pendingResponses++;
        if (connection->callbacks.pending) {
            connection->callbacks.pending(connection->callbacks.context,
                                          connection, response.handlerContext);
        }
        return false;
    }
    return bb_connection_finish(connection, &response);
}

static void bb_connection_service(BBConnection *connection)
{
    while (connection->state == BBConnectionStateHTTP &&
           connection->inputLength != 0) {
        if (!bb_connection_service_one(connection)) break;
    }
}

bool bb_connection_receive(BBConnection *connection,
                           const unsigned char *bytes,
                           size_t length)
{
    if (!connection || (length && !bytes)) return false;
    if (connection->state == BBConnectionStateClosing) return false;
    if (connection->state == BBConnectionStateUpgraded) {
        bb_connection_deliver_upgraded(connection, bytes, length);
        return connection->state != BBConnectionStateClosing;
    }
    if (connection->state == BBConnectionStateStreaming) {
        size_t take = length < connection->streamRemaining ? length : connection->streamRemaining;
        if (!bb_connection_stream_deliver(connection, bytes, take)) return false;
        bytes += take;
        length -= take;
        /* Pipelined bytes after the body are buffered before the response
         * goes out, so a pending completion finds them in place. */
        if (length > connection->inputCapacity - connection->inputLength) {
            bb_connection_reject(connection, 413,
                                 "Request exceeds the connection buffer");
            return false;
        }
        if (length) {
            bb_connection_memory_copy(connection->input + connection->inputLength,
                                      bytes, length);
            connection->inputLength += length;
        }
        if (!connection->streamRemaining) (void)bb_connection_stream_end(connection);
        bb_connection_service(connection);
        return connection->state != BBConnectionStateClosing;
    }
    if (length > connection->inputCapacity - connection->inputLength) {
        bb_connection_reject(connection, 413,
                             "Request exceeds the connection buffer");
        return false;
    }
    if (length) {
        bb_connection_memory_copy(connection->input + connection->inputLength,
                                  bytes, length);
        connection->inputLength += length;
    }
    bb_connection_service(connection);
    return connection->state != BBConnectionStateClosing;
}

bool bb_connection_complete(BBConnection *connection,
                            const BBHTTPResponse *response)
{
    if (!connection || !response) return false;
    if (connection->state != BBConnectionStatePending) return false;
    if (bb_connection_finish(connection, response)) {
        bb_connection_service(connection);
    }
    return connection->state != BBConnectionStateClosing;
}

const unsigned char *bb_connection_pending_body(const BBConnection *connection,
                                                size_t *bodyLength)
{
    if (!connection || connection->state != BBConnectionStatePending) {
        if (bodyLength) *bodyLength = 0;
        return NULL;
    }
    if (bodyLength) *bodyLength = connection->pendingBodyLength;
    return connection->input + connection->request.bodyOffset;
}

bool bb_connection_send_upgraded(BBConnection *connection,
                                 const unsigned char *bytes,
                                 size_t length)
{
    if (!connection || connection->state != BBConnectionStateUpgraded ||
        (length && !bytes)) return false;
    return bb_connection_emit(connection, bytes, length);
}
