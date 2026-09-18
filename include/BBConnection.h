#ifndef BB_CONNECTION_H
#define BB_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>

#include "BBHTTP.h"
#include "BBResponse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-socket HTTP state. Every connection owns its own input buffer, parsed
 * request, and response head; there is no process-global request slot. The
 * module allocates nothing: the caller supplies the input and scratch
 * buffers and receives response bytes through a callback. */

typedef enum {
    BBConnectionStateHTTP = 0,   /* parsing HTTP/1.x requests */
    BBConnectionStatePending,    /* a handler owes a response; input is held */
    BBConnectionStateUpgraded,   /* after a 101; bytes bypass the HTTP parser */
    BBConnectionStateStreaming,  /* a Content-Length body flows to streamData */
    BBConnectionStateClosing     /* response emitted; caller should close */
} BBConnectionState;

typedef struct BBConnection BBConnection;

/* Produce a response for one complete request. body points at the decoded
 * body inside the input buffer. Return false to answer 500 and close. */
typedef bool (*BBConnectionRequestHandler)(void *context,
                                           BBConnection *connection,
                                           const BBHTTPRequest *request,
                                           const unsigned char *body,
                                           size_t bodyLength,
                                           char *scratch,
                                           size_t scratchCapacity,
                                           BBHTTPResponse *response);

/* Emit response bytes. Return false if the socket is gone; the connection
 * then moves to the closing state. */
typedef bool (*BBConnectionEmit)(void *context,
                                 BBConnection *connection,
                                 const unsigned char *bytes,
                                 size_t length);

/* Bytes received after a successful 101 upgrade. Optional; without it the
 * upgraded connection is closed. */
typedef void (*BBConnectionUpgradedData)(void *context,
                                         BBConnection *connection,
                                         const unsigned char *bytes,
                                         size_t length);

/* Called once, after the 101 head has been emitted and before any trailing
 * bytes are delivered. handlerContext is the response's handlerContext. */
typedef void (*BBConnectionUpgraded)(void *context,
                                     BBConnection *connection,
                                     void *handlerContext);

/* Called when a handler returned a pending response. handlerContext is the
 * response's handlerContext; the owner later calls bb_connection_complete. */
typedef void (*BBConnectionPending)(void *context,
                                    BBConnection *connection,
                                    void *handlerContext);

/* Body streaming (uploads larger than the input buffer). Once a request
 * head with a Content-Length body is parsed, streamBegin may claim the
 * body: return true and set *streamContext. The body bytes then reach
 * streamData in order (the part already buffered first) and never
 * accumulate in the input buffer; streamEnd produces the response (which
 * may be pending) exactly like handleRequest, with an empty body. All
 * three are optional together; without streamBegin bodies are buffered.
 * Chunked bodies are never offered. */
typedef bool (*BBConnectionStreamBegin)(void *context,
                                        BBConnection *connection,
                                        const BBHTTPRequest *request,
                                        void **streamContext);

/* Return false to abandon the upload: the connection answers 400 and
 * closes (after streamAbort). */
typedef bool (*BBConnectionStreamData)(void *context,
                                       BBConnection *connection,
                                       void *streamContext,
                                       const unsigned char *bytes,
                                       size_t length);

typedef bool (*BBConnectionStreamEnd)(void *context,
                                      BBConnection *connection,
                                      void *streamContext,
                                      char *scratch,
                                      size_t scratchCapacity,
                                      BBHTTPResponse *response);

/* The stream ended without streamEnd (refused data, socket closed via
 * bb_connection_abort, or a failed streamEnd). Release streamContext. */
typedef void (*BBConnectionStreamAbort)(void *context,
                                        BBConnection *connection,
                                        void *streamContext);

typedef struct {
    BBConnectionRequestHandler handleRequest;
    BBConnectionEmit emit;
    BBConnectionUpgradedData upgradedData;
    BBConnectionUpgraded upgraded;
    BBConnectionPending pending;
    BBConnectionStreamBegin streamBegin;
    BBConnectionStreamData streamData;
    BBConnectionStreamEnd streamEnd;
    BBConnectionStreamAbort streamAbort;
    void *context;
} BBConnectionCallbacks;

struct BBConnection {
    BBConnectionState state;
    unsigned char *input;
    size_t inputCapacity;
    size_t inputLength;
    char *scratch;
    size_t scratchCapacity;
    BBConnectionCallbacks callbacks;
    BBHTTPRequest request;
    size_t pendingBodyLength;
    /* Streaming state: bytes of the body still expected. */
    void *streamContext;
    size_t streamRemaining;
    char head[BB_RESPONSE_MAX_HEAD_BYTES];
    /* Count-only diagnostics; never carry request content. */
    unsigned long requestsServed;
    unsigned long responsesEmitted;
    unsigned long requestsRejected;
    unsigned long upgradedBytes;
    unsigned long pendingResponses;
};

/* scratch must hold at least 256 bytes for error envelopes. */
bool bb_connection_init(BBConnection *connection,
                        unsigned char *input,
                        size_t inputCapacity,
                        char *scratch,
                        size_t scratchCapacity,
                        const BBConnectionCallbacks *callbacks);

/* Feed bytes from the socket. Complete requests are parsed one at a time,
 * answered through the callbacks, and consumed; partial input and pipelined
 * bytes stay buffered. Returns false once the connection should be closed. */
bool bb_connection_receive(BBConnection *connection,
                           const unsigned char *bytes,
                           size_t length);

/* Answer the request a handler left pending. The request and decoded body
 * remain readable through connection->request until this call returns.
 * Pipelined bytes received meanwhile are then parsed. Returns false once
 * the connection should be closed. */
bool bb_connection_complete(BBConnection *connection,
                            const BBHTTPResponse *response);

/* The socket went away. Aborts an in-flight stream (streamAbort) and
 * moves to the closing state; safe to call in any state. */
void bb_connection_abort(BBConnection *connection);

bool bb_connection_wants_close(const BBConnection *connection);
BBConnectionState bb_connection_state(const BBConnection *connection);
/* Decoded body of the pending request, or NULL when nothing is pending. */
const unsigned char *bb_connection_pending_body(const BBConnection *connection,
                                                size_t *bodyLength);

/* Emit a server-initiated frame on an upgraded connection. */
bool bb_connection_send_upgraded(BBConnection *connection,
                                 const unsigned char *bytes,
                                 size_t length);

#ifdef __cplusplus
}
#endif

#endif
