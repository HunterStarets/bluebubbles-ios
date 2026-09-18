#ifndef BB_REQUEST_TABLE_H
#define BB_REQUEST_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "BBConnection.h"
#include "BBEngineIO.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Correlation between transport requests and MobileSMS bridge replies.
 *
 * Every request the SpringBoard side forwards over IPC gets a slot keyed by
 * a unique requestID. A reply completes exactly that slot: for an HTTP
 * origin it becomes the pending connection's response, for a Socket.IO
 * origin it becomes the ACK. Events never touch this table (see BBEvents),
 * so an event can never consume or overwrite a pending request.
 *
 * The table allocates nothing and owns no timers; the caller drives
 * expiry with bb_request_table_expire. */

#define BB_REQUEST_MAX_OPERATION_BYTES 32U
#define BB_REQUEST_MAX_ARGUMENTS_BYTES 8192U
#define BB_REQUEST_DEFAULT_TIMEOUT_MS UINT64_C(30000)

typedef enum {
    BBRequestOriginNone = 0,
    BBRequestOriginHTTP,
    BBRequestOriginSocketAck
} BBRequestOrigin;

typedef struct {
    bool used;
    bool dispatched;
    uint64_t requestID;
    BBRequestOrigin origin;
    BBConnection *connection;   /* HTTP origin */
    BBEIOSession *session;      /* Socket.IO origin */
    uint64_t ackID;
    uint64_t openedAt;
    char operation[BB_REQUEST_MAX_OPERATION_BYTES];
    char argumentsJSON[BB_REQUEST_MAX_ARGUMENTS_BYTES];
    size_t argumentsLength;
} BBRequest;

typedef struct BBRequestTable BBRequestTable;

/* Forward the request to the bridge (IPC on the device; a fake bridge in
 * host tests). Return false if it could not be sent; the table then
 * completes the request with 503. */
typedef bool (*BBRequestDispatch)(void *context, BBRequestTable *table,
                                  const BBRequest *request);

struct BBRequestTable {
    BBRequest *requests;
    size_t capacity;
    uint64_t nextRequestID;
    uint64_t timeoutMs;
    BBRequestDispatch dispatch;
    void *context;
    /* Staging for Socket.IO ACK envelopes; must not alias session buffers. */
    char *scratch;
    size_t scratchCapacity;
    /* Count-only diagnostics. */
    unsigned long opened;
    unsigned long completed;
    unsigned long expired;
    unsigned long cancelled;
    unsigned long refused;
};

/* firstRequestID should carry a per-process nonce so ids never repeat
 * across SpringBoard restarts. */
bool bb_request_table_init(BBRequestTable *table,
                           BBRequest *requests, size_t capacity,
                           uint64_t firstRequestID, uint64_t timeoutMs,
                           BBRequestDispatch dispatch, void *context,
                           char *scratch, size_t scratchCapacity);

/* Claim a slot for a request that will answer an HTTP pending response.
 * The caller dispatches once the connection has entered the pending state
 * (from the connection's pending callback). NULL when the table is full. */
BBRequest *bb_request_table_open_http(BBRequestTable *table,
                                      BBConnection *connection,
                                      const char *operation,
                                      const char *argumentsJSON,
                                      size_t argumentsLength,
                                      uint64_t nowMs);

/* Claim a slot that will answer a Socket.IO ACK. */
BBRequest *bb_request_table_open_ack(BBRequestTable *table,
                                     BBEIOSession *session,
                                     uint64_t ackID,
                                     const char *operation,
                                     const char *argumentsJSON,
                                     size_t argumentsLength,
                                     uint64_t nowMs);

/* Send the request through the dispatch callback. On refusal the request
 * is completed with a 503 envelope. Returns whether dispatch succeeded. */
bool bb_request_table_dispatch(BBRequestTable *table, BBRequest *request);

/* Deliver a bridge reply. dataJSON, metadataJSON, and errorJSON are raw
 * JSON or NULL/0 and are validated before use. Returns false when no such
 * request is pending. */
bool bb_request_table_complete(BBRequestTable *table,
                               uint64_t requestID,
                               int status,
                               const char *message,
                               const char *dataJSON, size_t dataLength,
                               const char *metadataJSON, size_t metadataLength,
                               const char *errorJSON, size_t errorLength);

/* Complete every request older than the timeout with 504. */
size_t bb_request_table_expire(BBRequestTable *table, uint64_t nowMs);

/* Complete an HTTP-origin request with a raw response body (attachment
 * download), bypassing the JSON envelope. contentType may be NULL. body
 * is copied by the connection before this returns, so the caller may free
 * it immediately after. Socket.IO-origin or unknown requests return false;
 * the slot is released either way when the id matches. */
bool bb_request_table_complete_raw(BBRequestTable *table,
                                   uint64_t requestID,
                                   int status,
                                   const char *contentType,
                                   const unsigned char *body,
                                   size_t bodyLength);

/* Drop requests whose origin is gone without emitting anything. */
size_t bb_request_table_cancel_connection(BBRequestTable *table,
                                          const BBConnection *connection);
size_t bb_request_table_cancel_session(BBRequestTable *table,
                                       const BBEIOSession *session);

BBRequest *bb_request_table_find(const BBRequestTable *table, uint64_t requestID);
size_t bb_request_table_active_count(const BBRequestTable *table);

#ifdef __cplusplus
}
#endif

#endif
