#include "BBConnection.h"
#include "BBEngineIO.h"
#include "BBEvents.h"
#include "BBRequestTable.h"
#include "BBRouter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

#define TEST_PASSWORD "fixture-only"
#define SESSION_SLOTS 3
#define REQUEST_SLOTS 4
#define RECORDED_REQUESTS 16

typedef struct {
    BBConnection connection;
    unsigned char input[16384];
    char scratch[8192];
    unsigned char output[65536];
    size_t outputLength;
    BBEIOSession *session;
    unsigned pendingCalls;
    bool alive;
    BBRouterUpload upload;
} Socket;

/* The fake upload sink: one staged file in memory. */
#define STAGED_CAPACITY (128 * 1024)
typedef struct {
    unsigned char bytes[STAGED_CAPACITY];
    size_t length;
    bool open;
    bool kept;
    unsigned opens;
    unsigned closes;
    unsigned deletes;
    char filename[256];
    char contentType[128];
    bool failWrite;
} Staged;
static Staged staged;

/* What the fake bridge saw: one record per dispatched request. */
typedef struct {
    uint64_t requestID;
    char operation[BB_REQUEST_MAX_OPERATION_BYTES];
    char argumentsJSON[BB_REQUEST_MAX_ARGUMENTS_BYTES];
} Recorded;

typedef struct {
    BBEIOSession sessions[SESSION_SLOTS];
    unsigned char frames[SESSION_SLOTS][4096];
    unsigned char message[SESSION_SLOTS][4096];
    unsigned char outbound[SESSION_SLOTS][8192];
    Socket *socketForSlot[SESSION_SLOTS];
    BBEIOSessionTable table;
    BBRequest requests[REQUEST_SLOTS];
    BBRequestTable requestTable;
    char requestScratch[8192];
    BBRouterConfig config;
    unsigned sidCounter;
    uint64_t now;
    char eventScratch[4096];
    Recorded recorded[RECORDED_REQUESTS];
    size_t recordedCount;
    bool refuseDispatch;
    unsigned closedCalls;
} Harness;

static Harness *harness;

static size_t slot_index(const BBEIOSession *session)
{
    return (size_t)(session - harness->sessions);
}

static bool generate_sid(void *context, char *out, size_t capacity)
{
    snprintf(out, capacity, "sid-%u", ++((Harness *)context)->sidCounter);
    return true;
}

static uint64_t clock_now(void *context)
{
    return ((Harness *)context)->now;
}

static bool fake_dispatch(void *context, BBRequestTable *table, const BBRequest *request)
{
    Harness *owner = (Harness *)context;
    Recorded *record;
    (void)table;
    if (owner->refuseDispatch) return false;
    if (owner->recordedCount >= RECORDED_REQUESTS) return false;
    record = &owner->recorded[owner->recordedCount++];
    record->requestID = request->requestID;
    strcpy(record->operation, request->operation);
    strcpy(record->argumentsJSON, request->argumentsJSON);
    return true;
}

static Recorded *last_recorded(void)
{
    return harness->recordedCount ? &harness->recorded[harness->recordedCount - 1] : NULL;
}

static bool session_send_frame(void *context, BBEIOSession *session,
                               const unsigned char *bytes, size_t length)
{
    Socket *socket = harness->socketForSlot[slot_index(session)];
    (void)context;
    if (!socket) return false;
    return bb_connection_send_upgraded(&socket->connection, bytes, length);
}

static void session_event(void *context, BBEIOSession *session,
                          const unsigned char *packet, size_t packetLength,
                          const BBSIOPacket *parsed)
{
    Harness *owner = (Harness *)context;
    CHECK(bb_router_socket_event(&owner->config, session, packet, packetLength, parsed,
                                 owner->eventScratch, sizeof(owner->eventScratch)));
}

static void complete_polling_waiter(BBEIOSession *session)
{
    Socket *socket = (Socket *)session->pollingWaiter;
    BBHTTPResponse response;
    size_t length = 0;
    if (!socket) return;
    session->pollingWaiter = NULL;
    CHECK(bb_eio_session_drain(session, (unsigned char *)socket->scratch,
                               sizeof(socket->scratch), &length));
    bb_response_init(&response);
    response.contentType = BB_RESPONSE_CONTENT_TYPE_TEXT;
    response.body = (const unsigned char *)socket->scratch;
    response.bodyLength = length;
    socket->alive = bb_connection_complete(&socket->connection, &response);
}

static void session_polling_ready(void *context, BBEIOSession *session)
{
    (void)context;
    complete_polling_waiter(session);
}

static void session_closed(void *context, BBEIOSession *session, BBEIOCloseReason reason)
{
    Harness *owner = (Harness *)context;
    (void)reason;
    owner->closedCalls++;
    /* The owner drops any ACK the bridge still owes this session. */
    bb_request_table_cancel_session(&owner->requestTable, session);
    if (session->transport == BBEIOTransportPolling) complete_polling_waiter(session);
    bb_eio_table_release(session);
}

static bool socket_emit(void *context, BBConnection *connection,
                        const unsigned char *bytes, size_t length)
{
    Socket *socket = (Socket *)context;
    (void)connection;
    if (length > sizeof(socket->output) - socket->outputLength) return false;
    memcpy(socket->output + socket->outputLength, bytes, length);
    socket->outputLength += length;
    return true;
}

static bool socket_handle(void *context, BBConnection *connection,
                          const BBHTTPRequest *request,
                          const unsigned char *body, size_t bodyLength,
                          char *scratch, size_t scratchCapacity,
                          BBHTTPResponse *response)
{
    (void)context;
    return bb_router_route(&harness->config, connection, request, body, bodyLength,
                           scratch, scratchCapacity, response);
}

static void socket_upgraded(void *context, BBConnection *connection, void *handlerContext)
{
    Socket *socket = (Socket *)context;
    BBEIOSession *session = (BBEIOSession *)handlerContext;
    (void)connection;
    socket->session = session;
    harness->socketForSlot[slot_index(session)] = socket;
    CHECK(bb_eio_session_start(session));
}

static void socket_upgraded_data(void *context, BBConnection *connection,
                                 const unsigned char *bytes, size_t length)
{
    Socket *socket = (Socket *)context;
    (void)connection;
    if (socket->session) (void)bb_eio_session_receive_bytes(socket->session, bytes, length, harness->now);
}

/* The owner's pending callback: a bridge request is dispatched here, a
 * polling hold just remembers the connection. */
static void socket_pending(void *context, BBConnection *connection, void *handlerContext)
{
    Socket *socket = (Socket *)context;
    (void)connection;
    socket->pendingCalls++;
    if (bb_request_table_find(&harness->requestTable,
                              ((BBRequest *)handlerContext)->requestID) == handlerContext) {
        (void)bb_request_table_dispatch(&harness->requestTable, (BBRequest *)handlerContext);
    } else {
        ((BBEIOSession *)handlerContext)->pollingWaiter = socket;
    }
}

static bool staged_open(void *context, BBRouterUpload *upload, const char *filename,
                        const char *contentType)
{
    (void)context;
    if (staged.open) return false;
    staged.open = true;
    staged.opens++;
    staged.length = 0;
    staged.kept = false;
    strcpy(staged.filename, filename);
    strcpy(staged.contentType, contentType);
    strcpy(upload->uploadPath, "/tmp/bbupload-fixture");
    return true;
}

static bool staged_write(void *context, BBRouterUpload *upload, const unsigned char *bytes,
                         size_t length)
{
    (void)context;
    (void)upload;
    if (!staged.open || staged.failWrite) return false;
    if (staged.length + length > STAGED_CAPACITY) return false;
    memcpy(staged.bytes + staged.length, bytes, length);
    staged.length += length;
    return true;
}

static void staged_close(void *context, BBRouterUpload *upload, bool keep)
{
    (void)context;
    (void)upload;
    CHECK(staged.open);
    staged.open = false;
    staged.closes++;
    staged.kept = keep;
    if (!keep) staged.deletes++;
}

static bool socket_stream_begin(void *context, BBConnection *connection,
                                const BBHTTPRequest *request, void **streamContext)
{
    Socket *socket = (Socket *)context;
    if (!bb_router_upload_begin(&harness->config, connection, request, &socket->upload))
        return false;
    *streamContext = &socket->upload;
    return true;
}

static bool socket_stream_data(void *context, BBConnection *connection, void *streamContext,
                               const unsigned char *bytes, size_t length)
{
    (void)context;
    (void)connection;
    return bb_router_upload_data((BBRouterUpload *)streamContext, bytes, length);
}

static bool socket_stream_end(void *context, BBConnection *connection, void *streamContext,
                              char *scratch, size_t scratchCapacity, BBHTTPResponse *response)
{
    (void)context;
    (void)connection;
    return bb_router_upload_end((BBRouterUpload *)streamContext, scratch, scratchCapacity, response);
}

static void socket_stream_abort(void *context, BBConnection *connection, void *streamContext)
{
    (void)context;
    (void)connection;
    bb_router_upload_abort((BBRouterUpload *)streamContext);
}

static void socket_init(Socket *socket)
{
    BBConnectionCallbacks callbacks;
    memset(socket, 0, sizeof(*socket));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.handleRequest = socket_handle;
    callbacks.emit = socket_emit;
    callbacks.upgradedData = socket_upgraded_data;
    callbacks.upgraded = socket_upgraded;
    callbacks.pending = socket_pending;
    callbacks.streamBegin = socket_stream_begin;
    callbacks.streamData = socket_stream_data;
    callbacks.streamEnd = socket_stream_end;
    callbacks.streamAbort = socket_stream_abort;
    callbacks.context = socket;
    CHECK(bb_connection_init(&socket->connection, socket->input, sizeof(socket->input),
                             socket->scratch, sizeof(socket->scratch), &callbacks));
    socket->alive = true;
}

static void harness_init(void)
{
    memset(harness, 0, sizeof(*harness));
    for (size_t i = 0; i < SESSION_SLOTS; i++) {
        bb_eio_session_set_buffers(&harness->sessions[i],
                                   harness->frames[i], sizeof(harness->frames[i]),
                                   harness->message[i], sizeof(harness->message[i]),
                                   harness->outbound[i], sizeof(harness->outbound[i]));
    }
    harness->table.sessions = harness->sessions;
    harness->table.capacity = SESSION_SLOTS;
    harness->table.generateSid = generate_sid;
    harness->table.context = harness;
    harness->now = 5000000;
    CHECK(bb_request_table_init(&harness->requestTable, harness->requests, REQUEST_SLOTS,
                                UINT64_C(0x1000000000), 30000, fake_dispatch, harness,
                                harness->requestScratch, sizeof(harness->requestScratch)));

    harness->config.password = TEST_PASSWORD;
    harness->config.metadata.computerID = "bluebubbles-ios-host-test";
    harness->config.sessions = &harness->table;
    harness->config.sessionCallbacks.event = session_event;
    harness->config.sessionCallbacks.sendFrame = session_send_frame;
    harness->config.sessionCallbacks.pollingReady = session_polling_ready;
    harness->config.sessionCallbacks.closed = session_closed;
    harness->config.sessionCallbacks.context = harness;
    harness->config.requests = &harness->requestTable;
    harness->config.nowMs = clock_now;
    harness->config.clockContext = harness;
    harness->config.uploads.openFile = staged_open;
    harness->config.uploads.writeFile = staged_write;
    harness->config.uploads.closeFile = staged_close;
    harness->config.uploads.context = harness;
    memset(&staged, 0, sizeof(staged));
}

static bool feed_text(Socket *socket, const char *text)
{
    socket->alive = bb_connection_receive(&socket->connection, (const unsigned char *)text,
                                          strlen(text));
    return socket->alive;
}

static const char *output_text(Socket *socket)
{
    socket->output[socket->outputLength] = '\0';
    return (const char *)socket->output;
}

static bool output_has(Socket *socket, const char *needle)
{
    return strstr(output_text(socket), needle) != NULL;
}

static void reset_output(Socket *socket)
{
    socket->outputLength = 0;
}

static bool complete(uint64_t requestID, int status, const char *message,
                     const char *data, const char *metadata, const char *error)
{
    return bb_request_table_complete(&harness->requestTable, requestID, status, message,
                                     data, data ? strlen(data) : 0,
                                     metadata, metadata ? strlen(metadata) : 0,
                                     error, error ? strlen(error) : 0);
}

static void test_chat_query_round_trip(void)
{
    Socket *socket = malloc(sizeof(*socket));
    Recorded *record;
    harness_init();
    socket_init(socket);

    CHECK(feed_text(socket, "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                            "Content-Type: application/json\r\nContent-Length: 74\r\n\r\n"
                            "{\"with\":[\"participants\",\"lastmessage\"],\"offset\":0,\"limit\":100,\"sort\":null}"));
    CHECK(socket->outputLength == 0);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStatePending);
    CHECK(socket->pendingCalls == 1);
    CHECK(harness->recordedCount == 1);
    record = last_recorded();
    CHECK(strcmp(record->operation, "chat.query") == 0);
    CHECK(strcmp(record->argumentsJSON,
        "{\"withParticipants\":true,\"withLastMessage\":true,\"withArchived\":false,"
        "\"withSMS\":false,\"sort\":null,\"offset\":0,\"limit\":100}") == 0);
    CHECK(record->requestID == UINT64_C(0x1000000000));
    CHECK(bb_request_table_active_count(&harness->requestTable) == 1);

    /* The bridge replies with serialized chats plus pagination metadata. */
    CHECK(complete(record->requestID, 200, "Success",
                   "[{\"originalROWID\":1,\"guid\":\"iMessage;-;+15555550100\",\"style\":45,"
                   "\"participants\":[{\"originalROWID\":101,\"address\":\"+15555550100\"}]}]",
                   "{\"offset\":0,\"limit\":100,\"total\":1,\"count\":1}", NULL));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(output_has(socket, "Content-Type: application/json; charset=utf-8\r\n"));
    CHECK(output_has(socket,
        "\r\n\r\n{\"status\":200,\"message\":\"Success\",\"data\":[{\"originalROWID\":1,"
        "\"guid\":\"iMessage;-;+15555550100\",\"style\":45,\"participants\":[{\"originalROWID\":101,"
        "\"address\":\"+15555550100\"}]}],\"metadata\":{\"offset\":0,\"limit\":100,\"total\":1,\"count\":1}}"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
    CHECK(bb_request_table_active_count(&harness->requestTable) == 0);
    CHECK(harness->requestTable.completed == 1);
    /* Completing twice or with an unknown id is refused. */
    CHECK(!complete(record->requestID, 200, "Success", "[]", NULL, NULL));
    CHECK(!complete(12345, 200, "Success", "[]", NULL, NULL));

    /* An empty body means defaults; a string "with" is accepted too. */
    reset_output(socket);
    CHECK(feed_text(socket, "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                            "Content-Length: 0\r\n\r\n"));
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"withParticipants\":true,\"withLastMessage\":false,\"withArchived\":false,"
        "\"withSMS\":false,\"sort\":null,\"offset\":0,\"limit\":100}") == 0);
    CHECK(complete(last_recorded()->requestID, 200, "Success", "[]",
                   "{\"offset\":0,\"limit\":100,\"total\":0,\"count\":0}", NULL));
    reset_output(socket);
    CHECK(feed_text(socket, "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                            "Content-Length: 66\r\n\r\n"
                            "{\"with\":\"participants, lastMessage,archived\",\"sort\":\"lastmessage\"}"));
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"withParticipants\":true,\"withLastMessage\":true,\"withArchived\":true,"
        "\"withSMS\":false,\"sort\":\"lastmessage\",\"offset\":0,\"limit\":100}") == 0);
    CHECK(complete(last_recorded()->requestID, 200, "Success", "[]", NULL, NULL));
    CHECK(output_has(socket, "\"data\":[]}"));
    free(socket);
}

static void test_out_of_order_completion(void)
{
    Socket *a = malloc(sizeof(*a));
    Socket *b = malloc(sizeof(*b));
    Socket *c = malloc(sizeof(*c));
    uint64_t idA, idB, idC;
    harness_init();
    socket_init(a);
    socket_init(b);
    socket_init(c);

    CHECK(feed_text(a, "POST /api/v1/message/query?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                       "Content-Length: 214\r\n\r\n"
                       "{\"with\":[\"chats\",\"chats.participants\",\"attachments\",\"handle\","
                       "\"attributedBody\",\"messageSummaryInfo\",\"payloadData\"],\"where\":[],"
                       "\"sort\":\"DESC\",\"after\":1758067200123,\"before\":null,\"chatGuid\":null,"
                       "\"offset\":0,\"limit\":100}"));
    idA = last_recorded()->requestID;
    CHECK(strcmp(last_recorded()->operation, "message.query") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"chatGuid\":null,\"withChats\":true,\"withChatParticipants\":true,"
        "\"withAttachments\":true,\"withHandle\":true,\"sort\":\"DESC\","
        "\"after\":1758067200123,\"before\":null,\"offset\":0,\"limit\":100}") == 0);

    CHECK(feed_text(b, "GET /api/v1/chat/count?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    idB = last_recorded()->requestID;
    CHECK(strcmp(last_recorded()->operation, "chat.count") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON, "{\"includeArchived\":false}") == 0);

    /* Percent-encoded chat guid in the path, client history query string. */
    CHECK(feed_text(c, "GET /api/v1/chat/iMessage%3B-%3B%2B15555550100/message?guid=" TEST_PASSWORD
                       "&with=attachments,handle&sort=DESC&after=0&offset=0&limit=25 HTTP/1.1\r\n\r\n"
                       "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    idC = last_recorded()->requestID;
    CHECK(strcmp(last_recorded()->operation, "chat.messages") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"guid\":\"iMessage;-;+15555550100\",\"withAttachments\":true,\"withHandle\":true,"
        "\"sort\":\"DESC\",\"after\":0,\"before\":null,\"offset\":0,\"limit\":25}") == 0);
    CHECK(idA < idB && idB < idC);
    CHECK(bb_request_table_active_count(&harness->requestTable) == 3);
    CHECK(a->outputLength == 0 && b->outputLength == 0 && c->outputLength == 0);

    /* Replies arrive C, A, B; each lands on its own connection. */
    CHECK(complete(idC, 200, "Successfully fetched messages!",
                   "[{\"guid\":\"m-c\",\"dateCreated\":1758067200123}]",
                   "{\"offset\":0,\"limit\":25,\"total\":1,\"count\":1}", NULL));
    CHECK(output_has(c, "\"message\":\"Successfully fetched messages!\",\"data\":[{\"guid\":\"m-c\""));
    /* The pipelined ping behind C's history is served after completion. */
    CHECK(output_has(c, "\"data\":\"pong\""));
    CHECK(a->outputLength == 0 && b->outputLength == 0);
    CHECK(complete(idA, 200, "Success", "[]", "{\"offset\":0,\"limit\":100,\"total\":0,\"count\":0}", NULL));
    CHECK(output_has(a, "\"data\":[],\"metadata\":{\"offset\":0"));
    CHECK(b->outputLength == 0);
    CHECK(complete(idB, 200, "Success", "{\"total\":7,\"breakdown\":{\"iMessage\":5,\"SMS\":2}}", NULL, NULL));
    CHECK(output_has(b, "\"data\":{\"total\":7,\"breakdown\":{\"iMessage\":5,\"SMS\":2}}}"));
    CHECK(bb_request_table_active_count(&harness->requestTable) == 0);
    free(a);
    free(b);
    free(c);
}

static void test_error_and_failure_replies(void)
{
    Socket *socket = malloc(sizeof(*socket));
    static char big[9000];
    harness_init();
    socket_init(socket);

    /* Bridge error envelopes pass through with their status. */
    CHECK(feed_text(socket, "GET /api/v1/chat/nope?guid=" TEST_PASSWORD "&with=participants HTTP/1.1\r\n\r\n"));
    CHECK(strcmp(last_recorded()->operation, "chat.get") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"guid\":\"nope\",\"withParticipants\":true,\"withLastMessage\":false}") == 0);
    CHECK(complete(last_recorded()->requestID, 404, "Not Found", NULL, NULL,
                   "{\"type\":\"Database Error\",\"message\":\"Chat does not exist!\"}"));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(output_has(socket, "\r\n\r\n{\"status\":404,\"message\":\"Not Found\",\"error\":"
                             "{\"type\":\"Database Error\",\"message\":\"Chat does not exist!\"}}"));

    /* A malformed reply never reaches the client as-is. */
    reset_output(socket);
    CHECK(feed_text(socket, "GET /api/v1/attachment/att-1?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(strcmp(last_recorded()->argumentsJSON, "{\"guid\":\"att-1\"}") == 0);
    CHECK(complete(last_recorded()->requestID, 200, "Success", "{\"guid\":", NULL, NULL));
    CHECK(output_has(socket, "HTTP/1.1 500 ") && output_has(socket, "Malformed bridge reply"));
    reset_output(socket);
    CHECK(feed_text(socket, "GET /api/v1/message/m-1?guid=" TEST_PASSWORD "&with=chats,attachments HTTP/1.1\r\n\r\n"));
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"guid\":\"m-1\",\"withChats\":true,\"withAttachments\":true,\"withHandle\":true}") == 0);
    CHECK(complete(last_recorded()->requestID, 999, NULL, "{}", NULL, NULL));
    CHECK(output_has(socket, "HTTP/1.1 500 ") && output_has(socket, "Malformed bridge reply"));

    /* A reply larger than the connection scratch is refused truthfully. */
    reset_output(socket);
    CHECK(feed_text(socket, "GET /api/v1/message/count?guid=" TEST_PASSWORD "&after=1758067200123 HTTP/1.1\r\n\r\n"));
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"chatGuid\":null,\"after\":1758067200123,\"before\":null}") == 0);
    big[0] = '"';
    memset(big + 1, 'x', sizeof(big) - 3);
    big[sizeof(big) - 2] = '"';
    big[sizeof(big) - 1] = '\0';
    CHECK(complete(last_recorded()->requestID, 200, "Success", big, NULL, NULL));
    CHECK(output_has(socket, "HTTP/1.1 500 ") && output_has(socket, "Reply exceeds the response buffer"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);

    /* Timeout: the caller drives expiry. */
    reset_output(socket);
    CHECK(feed_text(socket, "GET /api/v1/chat/count?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(bb_request_table_expire(&harness->requestTable, harness->now + 29999) == 0);
    CHECK(socket->outputLength == 0);
    CHECK(bb_request_table_expire(&harness->requestTable, harness->now + 30000) == 1);
    CHECK(output_has(socket, "HTTP/1.1 504 Gateway Timeout\r\n"));
    CHECK(output_has(socket, "{\"status\":504,\"message\":\"Gateway Timeout\",\"error\":"
                             "{\"type\":\"Gateway Timeout\",\"message\":\"The messaging bridge did not reply in time\"}}"));
    CHECK(harness->requestTable.expired == 1);

    /* Dispatch refusal completes the pending connection with 503. */
    reset_output(socket);
    harness->refuseDispatch = true;
    CHECK(feed_text(socket, "GET /api/v1/chat/count?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 503 Service Unavailable\r\n"));
    CHECK(output_has(socket, "The messaging bridge is unavailable"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
    CHECK(bb_request_table_active_count(&harness->requestTable) == 0);
    harness->refuseDispatch = false;

    /* Connection lost while pending: the owner cancels; the late reply is refused. */
    reset_output(socket);
    CHECK(feed_text(socket, "GET /api/v1/chat/count?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    {
        uint64_t id = last_recorded()->requestID;
        CHECK(bb_request_table_cancel_connection(&harness->requestTable, &socket->connection) == 1);
        CHECK(bb_request_table_active_count(&harness->requestTable) == 0);
        CHECK(!complete(id, 200, "Success", "{}", NULL, NULL));
        CHECK(socket->outputLength == 0);
    }
    free(socket);
}

static void test_table_exhaustion_and_validation(void)
{
    Socket *sockets[REQUEST_SLOTS + 1];
    harness_init();
    for (size_t i = 0; i <= REQUEST_SLOTS; i++) {
        sockets[i] = malloc(sizeof(*sockets[i]));
        socket_init(sockets[i]);
        CHECK(feed_text(sockets[i], "GET /api/v1/chat/count?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    }
    CHECK(bb_request_table_active_count(&harness->requestTable) == REQUEST_SLOTS);
    CHECK(output_has(sockets[REQUEST_SLOTS], "HTTP/1.1 503 Service Unavailable\r\n"));
    CHECK(output_has(sockets[REQUEST_SLOTS], "Too many pending requests"));
    CHECK(bb_connection_state(&sockets[REQUEST_SLOTS]->connection) == BBConnectionStateHTTP);
    CHECK(harness->requestTable.refused == 1);
    for (size_t i = 0; i < REQUEST_SLOTS; i++) {
        CHECK(complete(harness->recorded[i].requestID, 200, "Success", "{\"total\":0}", NULL, NULL));
        CHECK(output_has(sockets[i], "\"data\":{\"total\":0}}"));
    }

    /* Validation failures answer 400 without touching the bridge. */
    {
        Socket *socket = sockets[0];
        size_t before = harness->recordedCount;
        const char *bad[] = {
            "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 11\r\n\r\n{\"limit\":0}",
            "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 14\r\n\r\n{\"limit\":1001}",
            "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 13\r\n\r\n{\"offset\":-1}",
            "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 8\r\n\r\n{\"with\":",
            "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 2\r\n\r\n[]",
            "POST /api/v1/message/query?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 19\r\n\r\n{\"sort\":\"sideways\"}",
            "POST /api/v1/message/query?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 13\r\n\r\n{\"after\":\"1\"}",
            "GET /api/v1/chat/x/message?guid=" TEST_PASSWORD "&limit=abc HTTP/1.1\r\n\r\n",
            "GET /api/v1/chat/x/message?guid=" TEST_PASSWORD "&before=-5 HTTP/1.1\r\n\r\n",
            "GET /api/v1/chat/%ZZ/message?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            reset_output(socket);
            CHECK(feed_text(socket, bad[i]));
            if (!output_has(socket, "HTTP/1.1 400 Bad Request\r\n") && !output_has(socket, "HTTP/1.1 404 ")) {
                fprintf(stderr, "FAIL: expected 400/404 for request %zu\n", i);
                failures++;
            }
            CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
        }
        CHECK(harness->recordedCount == before);
        CHECK(bb_request_table_active_count(&harness->requestTable) == 0);

        /* Wrong methods on bridge routes. */
        reset_output(socket);
        CHECK(feed_text(socket, "POST /api/v1/chat/count?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 0\r\n\r\n"));
        CHECK(output_has(socket, "HTTP/1.1 405 "));
        reset_output(socket);
        CHECK(feed_text(socket, "GET /api/v1/message/query?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
        CHECK(output_has(socket, "HTTP/1.1 405 "));
        reset_output(socket);
        CHECK(feed_text(socket, "DELETE /api/v1/chat/x?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
        CHECK(output_has(socket, "HTTP/1.1 405 "));
        /* A guid longer than the bound is refused before dispatch. */
        {
            char request[1024];
            char guid[300];
            memset(guid, 'g', sizeof(guid) - 1);
            guid[sizeof(guid) - 1] = '\0';
            snprintf(request, sizeof(request), "GET /api/v1/chat/%s?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n", guid);
            reset_output(socket);
            CHECK(feed_text(socket, request));
            CHECK(output_has(socket, "HTTP/1.1 400 ") || output_has(socket, "HTTP/1.1 404 "));
            CHECK(harness->recordedCount == before);
        }
    }
    for (size_t i = 0; i <= REQUEST_SLOTS; i++) free(sockets[i]);
}

static const char handshake[] =
    "GET /socket.io/?EIO=4&transport=websocket&guid=" TEST_PASSWORD " HTTP/1.1\r\n"
    "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";

static size_t client_frame(unsigned char *out, const char *text)
{
    static const unsigned char mask[4] = { 1, 2, 3, 4 };
    size_t length = strlen(text);
    size_t cursor = 0;
    out[cursor++] = 0x81;
    if (length < 126) {
        out[cursor++] = (unsigned char)(0x80 | length);
    } else {
        out[cursor++] = 0x80 | 126;
        out[cursor++] = (unsigned char)(length >> 8);
        out[cursor++] = (unsigned char)length;
    }
    memcpy(out + cursor, mask, 4);
    cursor += 4;
    for (size_t i = 0; i < length; i++) out[cursor++] = (unsigned char)(text[i] ^ mask[i % 4]);
    return cursor;
}

static bool send_text_frame(Socket *socket, const char *text)
{
    unsigned char frame[4096];
    size_t length = client_frame(frame, text);
    socket->alive = bb_connection_receive(&socket->connection, frame, length);
    return socket->alive;
}

/* Payload of the last complete server frame in the output. */
static bool last_frame_payload(Socket *socket, char *payload, size_t capacity)
{
    size_t cursor = 0;
    bool found = false;
    while (cursor + 2 <= socket->outputLength) {
        unsigned char lengthCode = (unsigned char)(socket->output[cursor + 1] & 0x7f);
        size_t headerLength = 2;
        size_t length = lengthCode;
        if (lengthCode == 126) {
            length = ((size_t)socket->output[cursor + 2] << 8) | socket->output[cursor + 3];
            headerLength = 4;
        } else if (lengthCode == 127) {
            return false;
        }
        if (cursor + headerLength + length > socket->outputLength) break;
        if (length < capacity) {
            memcpy(payload, socket->output + cursor + headerLength, length);
            payload[length] = '\0';
            found = true;
        }
        cursor += headerLength + length;
    }
    return found;
}

static Socket *open_connected_websocket(void)
{
    Socket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    CHECK(feed_text(socket, handshake));
    CHECK(socket->session != NULL);
    CHECK(send_text_frame(socket, "40"));
    CHECK(socket->session->state == BBEIOSessionStateConnected);
    reset_output(socket);
    return socket;
}

static void test_socket_ack_round_trip(void)
{
    Socket *socket;
    char payload[4096];
    harness_init();
    socket = open_connected_websocket();

    CHECK(send_text_frame(socket, "4231[\"get-chats\",{\"withParticipants\":true,\"withLastMessage\":true,"
                                  "\"limit\":5,\"offset\":10,\"sort\":\"lastmessage\"}]"));
    CHECK(socket->outputLength == 0);
    CHECK(harness->recordedCount == 1);
    CHECK(strcmp(last_recorded()->operation, "chat.query") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"withParticipants\":true,\"withLastMessage\":true,\"withArchived\":false,"
        "\"withSMS\":false,\"sort\":\"lastmessage\",\"offset\":10,\"limit\":5}") == 0);
    CHECK(bb_request_table_find(&harness->requestTable, last_recorded()->requestID)->origin ==
          BBRequestOriginSocketAck);
    CHECK(complete(last_recorded()->requestID, 200, "Success", "[{\"guid\":\"c1\"}]",
                   "{\"offset\":10,\"limit\":5,\"total\":11,\"count\":1}", NULL));
    CHECK(last_frame_payload(socket, payload, sizeof(payload)));
    CHECK(strcmp(payload, "4331[{\"status\":200,\"message\":\"Success\",\"data\":[{\"guid\":\"c1\"}],"
                          "\"metadata\":{\"offset\":10,\"limit\":5,\"total\":11,\"count\":1}}]") == 0);
    CHECK(bb_request_table_active_count(&harness->requestTable) == 0);

    /* Other read events, including identifier aliases. */
    reset_output(socket);
    CHECK(send_text_frame(socket, "4232[\"get-chat-messages\",{\"identifier\":\"iMessage;-;+15555550100\","
                                  "\"limit\":25,\"after\":0,\"withAttachments\":true,\"withChatParticipants\":true}]"));
    CHECK(strcmp(last_recorded()->operation, "chat.messages") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"guid\":\"iMessage;-;+15555550100\",\"withAttachments\":true,\"withHandle\":true,"
        "\"sort\":\"DESC\",\"after\":0,\"before\":null,\"offset\":0,\"limit\":25}") == 0);
    CHECK(send_text_frame(socket, "4233[\"get-chat\",{\"chatGuid\":\"c1\"}]"));
    CHECK(strcmp(last_recorded()->operation, "chat.get") == 0);
    CHECK(send_text_frame(socket, "4234[\"get-messages\",{\"after\":5,\"limit\":50,\"withChats\":true}]"));
    CHECK(strcmp(last_recorded()->operation, "message.query") == 0);
    CHECK(strstr(last_recorded()->argumentsJSON, "\"withChats\":true,\"withChatParticipants\":false") != NULL);
    CHECK(send_text_frame(socket, "4235[\"get-attachment\",{\"guid\":\"att-1\"}]"));
    CHECK(strcmp(last_recorded()->operation, "attachment.get") == 0);
    CHECK(bb_request_table_active_count(&harness->requestTable) == 4);

    /* Validation errors are acknowledged immediately, without dispatch. */
    {
        size_t before = harness->recordedCount;
        CHECK(send_text_frame(socket, "4236[\"get-chat\",{}]"));
        CHECK(last_frame_payload(socket, payload, sizeof(payload)));
        CHECK(strcmp(payload, "4336[{\"status\":400,\"message\":\"Bad Request\",\"error\":"
                              "{\"type\":\"Validation Error\",\"message\":\"Missing identifier\"}}]") == 0);
        CHECK(send_text_frame(socket, "4237[\"get-chats\",{\"limit\":0}]"));
        CHECK(last_frame_payload(socket, payload, sizeof(payload)));
        CHECK(strstr(payload, "\"message\":\"Invalid limit\"") != NULL);
        CHECK(send_text_frame(socket, "4238[\"get-chats\",\"not-an-object\"]"));
        CHECK(last_frame_payload(socket, payload, sizeof(payload)));
        CHECK(strstr(payload, "Event arguments must be an object") != NULL);
        CHECK(harness->recordedCount == before);
    }

    /* Table full from the socket side. */
    CHECK(send_text_frame(socket, "4239[\"get-chats\",{}]"));
    CHECK(last_frame_payload(socket, payload, sizeof(payload)));
    CHECK(strncmp(payload, "4339[{\"status\":503,", 19) == 0);
    CHECK(strstr(payload, "Too many pending requests") != NULL);

    /* The session closes while replies are outstanding: they are dropped. */
    CHECK(send_text_frame(socket, "41"));
    CHECK(harness->closedCalls == 1);
    CHECK(bb_request_table_active_count(&harness->requestTable) == 0);
    CHECK(harness->requestTable.cancelled == 4);
    CHECK(!complete(harness->recorded[1].requestID, 200, "Success", "[]", NULL, NULL));
    free(socket);
}

static void test_events_are_independent(void)
{
    Socket *ws = malloc(sizeof(*ws));
    Socket *opening = malloc(sizeof(*opening));
    Socket *poll = malloc(sizeof(*poll));
    Socket *http = malloc(sizeof(*http));
    BBEIOSession *pollingSession;
    char payload[4096];
    uint64_t pendingID;
    harness_init();

    ws = open_connected_websocket();
    socket_init(opening);
    CHECK(feed_text(opening, handshake));
    CHECK(opening->session && opening->session->state == BBEIOSessionStateOpening);
    reset_output(opening);
    socket_init(poll);
    CHECK(feed_text(poll, "GET /socket.io/?EIO=4&transport=polling&guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    pollingSession = bb_eio_table_find(&harness->table, "sid-5");
    CHECK(pollingSession != NULL);
    CHECK(feed_text(poll, "POST /socket.io/?EIO=4&transport=polling&sid=sid-5&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 2\r\n\r\n40"));
    reset_output(poll);
    CHECK(feed_text(poll, "GET /socket.io/?EIO=4&transport=polling&sid=sid-5&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(poll, "40{\"sid\":\"sid-6\"}"));
    reset_output(poll);

    /* A bridge request is pending while an event arrives. */
    socket_init(http);
    CHECK(feed_text(http, "GET /api/v1/chat/count?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    pendingID = last_recorded()->requestID;
    CHECK(bb_request_table_active_count(&harness->requestTable) == 1);
    reset_output(ws);

    CHECK(bb_events_broadcast(&harness->table, "new-message",
                              "{\"guid\":\"m-9\",\"text\":\"hi\",\"chats\":[{\"guid\":\"c1\"}]}", 50) == 2);
    CHECK(last_frame_payload(ws, payload, sizeof(payload)));
    CHECK(strcmp(payload, "42[\"new-message\",{\"guid\":\"m-9\",\"text\":\"hi\",\"chats\":[{\"guid\":\"c1\"}]}]") == 0);
    CHECK(bb_eio_session_queued_bytes(pollingSession) > 0);
    CHECK(opening->outputLength == 0);

    /* Unsupported names and invalid payloads deliver nowhere. */
    reset_output(ws);
    CHECK(bb_events_broadcast(&harness->table, "participant-added", "{}", 2) == 0);
    CHECK(bb_events_broadcast(&harness->table, "new-message", "{bad", 4) == 0);
    CHECK(bb_events_broadcast(&harness->table, "new-message", "", 0) == 0);
    CHECK(bb_events_broadcast(NULL, "new-message", "{}", 2) == 0);
    CHECK(ws->outputLength == 0);
    CHECK(bb_events_name_supported("chat-read-status-changed"));
    CHECK(bb_events_name_supported("updated-message"));
    CHECK(!bb_events_name_supported("typing-indicator"));
    CHECK(!bb_events_name_supported("group-name-change"));

    /* The pending request is untouched by all of that and still completes. */
    CHECK(bb_request_table_active_count(&harness->requestTable) == 1);
    CHECK(http->outputLength == 0);
    CHECK(complete(pendingID, 200, "Success", "{\"total\":3}", NULL, NULL));
    CHECK(output_has(http, "\"data\":{\"total\":3}}"));

    /* The polling client reads the push on its next GET. */
    CHECK(feed_text(poll, "GET /socket.io/?EIO=4&transport=polling&sid=sid-5&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(poll, "\r\n\r\n42[\"new-message\",{\"guid\":\"m-9\""));
    free(ws);
    free(opening);
    free(poll);
    free(http);
}

static void test_message_send_route(void)
{
    Socket *socket = malloc(sizeof(*socket));
    harness_init();
    socket_init(socket);

    /* Valid send: becomes a pending message.send with chatGuid/message/tempGuid. */
    CHECK(feed_text(socket, "POST /api/v1/message/text?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                            "Content-Type: application/json\r\nContent-Length: 70\r\n\r\n"
                            "{\"chatGuid\":\"iMessage;-;+15555550100\",\"tempGuid\":\"t-1\",\"message\":\"hi\"}"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStatePending);
    CHECK(harness->recordedCount == 1);
    CHECK(strcmp(last_recorded()->operation, "message.send") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"chatGuid\":\"iMessage;-;+15555550100\",\"message\":\"hi\",\"tempGuid\":\"t-1\"}") == 0);
    CHECK(complete(last_recorded()->requestID, 200, "Message sent!",
                   "{\"guid\":\"sent-1\",\"tempGuid\":\"t-1\",\"text\":\"hi\",\"isFromMe\":true}", NULL, NULL));
    CHECK(output_has(socket, "HTTP/1.1 200 OK\r\n"));
    CHECK(output_has(socket, "\"tempGuid\":\"t-1\""));

    /* Validation: missing fields and unsupported options never reach the bridge. */
    {
        size_t before = harness->recordedCount;
        const char *bad[] = {
            "POST /api/v1/message/text?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}",
            "POST /api/v1/message/text?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 44\r\n\r\n{\"chatGuid\":\"x\",\"tempGuid\":\"t\",\"message\":\"\"}",
            "POST /api/v1/message/text?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 63\r\n\r\n{\"chatGuid\":\"x\",\"tempGuid\":\"t\",\"message\":\"h\",\"effectId\":\"slam\"}",
            "POST /api/v1/message/text?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 59\r\n\r\n{\"chatGuid\":\"x\",\"tempGuid\":\"t\",\"message\":\"h\",\"subject\":\"s\"}",
            "GET /api/v1/message/text?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n",
        };
        for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
            reset_output(socket);
            CHECK(feed_text(socket, bad[i]));
            if (!output_has(socket, "HTTP/1.1 400 ") && !output_has(socket, "HTTP/1.1 405 ")) {
                fprintf(stderr, "FAIL: expected 400/405 for send request %zu\n", i);
                failures++;
            }
        }
        CHECK(harness->recordedCount == before);
    }
    free(socket);
}

static void test_chat_new_route(void)
{
    Socket *socket = malloc(sizeof(*socket));
    Socket *ws;
    char payload[4096];
    harness_init();
    socket_init(socket);

    /* Stock client body: addresses + message + service + method (ignored). */
    CHECK(feed_text(socket, "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                            "Content-Type: application/json\r\nContent-Length: 93\r\n\r\n"
                            "{\"addresses\":[\"+15555550100\"],\"message\":\"hello\",\"service\":\"iMessage\","
                            "\"method\":\"apple-script\"}"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStatePending);
    CHECK(harness->recordedCount == 1);
    CHECK(strcmp(last_recorded()->operation, "chat.new") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"addresses\":[\"+15555550100\"],\"service\":\"iMessage\",\"message\":\"hello\","
        "\"tempGuid\":null}") == 0);
    CHECK(complete(last_recorded()->requestID, 200, "Successfully created chat!",
                   "{\"guid\":\"iMessage;-;+15555550100\",\"messages\":[{\"guid\":\"m-1\"}]}", NULL, NULL));
    CHECK(output_has(socket, "HTTP/1.1 200 OK\r\n"));
    CHECK(output_has(socket, "\"message\":\"Successfully created chat!\",\"data\":{\"guid\":\"iMessage;-;+15555550100\""));

    /* Group + SMS + tempGuid; service is case-insensitive. */
    reset_output(socket);
    CHECK(feed_text(socket, "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                            "Content-Length: 89\r\n\r\n"
                            "{\"addresses\":[\"+15555550100\",\"a@b.c\"],\"message\":\"hi\",\"service\":\"sms\","
                            "\"tempGuid\":\"temp-1\"}"));
    CHECK(strcmp(last_recorded()->operation, "chat.new") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"addresses\":[\"+15555550100\",\"a@b.c\"],\"service\":\"SMS\",\"message\":\"hi\","
        "\"tempGuid\":\"temp-1\"}") == 0);
    CHECK(complete(last_recorded()->requestID, 500, "Server Error", NULL, NULL,
                   "{\"type\":\"iMessage Error\",\"message\":\"Failed to create chat\"}"));
    CHECK(output_has(socket, "HTTP/1.1 500 "));

    /* Validation: nothing below reaches the bridge. */
    {
        size_t before = harness->recordedCount;
        const char *bad[] = {
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 3\r\n\r\n[1]",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 30\r\n\r\n{\"addresses\":[],\"message\":\"h\"}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 31\r\n\r\n{\"addresses\":[1],\"message\":\"h\"}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 32\r\n\r\n{\"addresses\":[\"\"],\"message\":\"h\"}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 19\r\n\r\n{\"addresses\":[\"x\"]}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 32\r\n\r\n{\"addresses\":[\"x\"],\"message\":\"\"}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 49\r\n\r\n{\"addresses\":[\"x\"],\"message\":\"h\",\"service\":\"RCS\"}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 51\r\n\r\n{\"addresses\":[\"x\"],\"message\":\"h\",\"effectId\":\"slam\"}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 47\r\n\r\n{\"addresses\":[\"x\"],\"message\":\"h\",\"subject\":\"s\"}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 53\r\n\r\n{\"addresses\":[\"x\"],\"message\":\"h\",\"attributedBody\":{}}",
            "POST /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 65\r\n\r\n{\"addresses\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"],\"message\":\"h\"}",
            "GET /api/v1/chat/new?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n",
        };
        for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
            reset_output(socket);
            CHECK(feed_text(socket, bad[i]));
            if (!output_has(socket, "HTTP/1.1 400 ") && !output_has(socket, "HTTP/1.1 405 ")) {
                fprintf(stderr, "FAIL: expected 400/405 for chat/new request %zu\n", i);
                failures++;
            }
        }
        CHECK(harness->recordedCount == before);
        /* The rejection names the iOS 9 limitation, not a generic error. */
        reset_output(socket);
        CHECK(feed_text(socket, bad[5]));
        CHECK(output_has(socket, "A message is required to create a chat on iOS 9"));
    }
    free(socket);

    /* Socket start-chat: participants may be a string or an array. */
    ws = open_connected_websocket();
    CHECK(send_text_frame(ws, "4240[\"start-chat\",{\"participants\":\"+15555550100\",\"message\":\"yo\","
                              "\"tempGuid\":\"t-9\"}]"));
    CHECK(strcmp(last_recorded()->operation, "chat.new") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"addresses\":[\"+15555550100\"],\"service\":\"iMessage\",\"message\":\"yo\","
        "\"tempGuid\":\"t-9\"}") == 0);
    CHECK(complete(last_recorded()->requestID, 200, "Successfully created chat!",
                   "{\"guid\":\"iMessage;-;+15555550100\"}", NULL, NULL));
    CHECK(last_frame_payload(ws, payload, sizeof(payload)));
    CHECK(strcmp(payload, "4340[{\"status\":200,\"message\":\"Successfully created chat!\","
                          "\"data\":{\"guid\":\"iMessage;-;+15555550100\"}}]") == 0);
    reset_output(ws);
    CHECK(send_text_frame(ws, "4241[\"start-chat\",{\"participants\":[\"a@b.c\",\"d@e.f\"],\"message\":\"yo\","
                              "\"service\":\"SMS\"}]"));
    CHECK(strstr(last_recorded()->argumentsJSON, "\"addresses\":[\"a@b.c\",\"d@e.f\"],\"service\":\"SMS\"") != NULL);
    {
        size_t before = harness->recordedCount;
        reset_output(ws);
        CHECK(send_text_frame(ws, "4242[\"start-chat\",{\"participants\":[]}]"));
        CHECK(last_frame_payload(ws, payload, sizeof(payload)));
        CHECK(strstr(payload, "\"status\":400") != NULL && strstr(payload, "Missing addresses") != NULL);
        reset_output(ws);
        CHECK(send_text_frame(ws, "4243[\"start-chat\"]"));
        CHECK(last_frame_payload(ws, payload, sizeof(payload)));
        CHECK(strstr(payload, "Missing addresses") != NULL);
        reset_output(ws);
        CHECK(send_text_frame(ws, "4244[\"start-chat\",{\"participants\":\"x\"}]"));
        CHECK(last_frame_payload(ws, payload, sizeof(payload)));
        CHECK(strstr(payload, "A message is required") != NULL);
        CHECK(harness->recordedCount == before);
    }
    free(ws);
}

/* Build a dio-shaped attachment upload: the file part first (as the stock
 * client orders its FormData), then the text fields. */
static size_t build_upload(unsigned char *out, size_t capacity, const char *query,
                           const unsigned char *file, size_t fileLength,
                           const char *extraFields, const char *contentType)
{
    const char boundary[] = "----dio-boundary-7f3a";
    char body[4096];
    size_t bodyLength;
    size_t headLength;
    char head[512];
    int written;
    written = snprintf(body, sizeof(body),
        "--%s\r\ncontent-disposition: form-data; name=\"attachment\"; filename=\"pic.jpg\"\r\n"
        "content-type: image/jpeg\r\n\r\n", boundary);
    bodyLength = (size_t)written;
    if (headLength = 0, bodyLength + fileLength + 1024 > capacity) return 0;
    {
        char tail[2048];
        written = snprintf(tail, sizeof(tail),
            "\r\n--%s\r\ncontent-disposition: form-data; name=\"chatGuid\"\r\n\r\niMessage;-;+15555550100"
            "\r\n--%s\r\ncontent-disposition: form-data; name=\"name\"\r\n\r\npic.jpg"
            "\r\n--%s\r\ncontent-disposition: form-data; name=\"method\"\r\n\r\napple-script"
            "%s"
            "\r\n--%s--\r\n", boundary, boundary, boundary, extraFields, boundary);
        headLength = (size_t)snprintf(head, sizeof(head),
            "POST /api/v1/message/attachment%s HTTP/1.1\r\nContent-Type: %s\r\n"
            "Content-Length: %zu\r\n\r\n", query,
            contentType ? contentType : "multipart/form-data; boundary=----dio-boundary-7f3a",
            bodyLength + fileLength + (size_t)written);
        memcpy(out, head, headLength);
        memcpy(out + headLength, body, bodyLength);
        memcpy(out + headLength + bodyLength, file, fileLength);
        memcpy(out + headLength + bodyLength + fileLength, tail, (size_t)written);
        return headLength + bodyLength + fileLength + (size_t)written;
    }
}

#define TEMP_FIELD "\r\n------dio-boundary-7f3a\r\ncontent-disposition: form-data; name=\"tempGuid\"\r\n\r\ntemp-9"

static void test_attachment_upload(void)
{
    Socket *socket = malloc(sizeof(*socket));
    static unsigned char file[40000];
    static unsigned char message[48000];
    size_t messageLength;
    harness_init();
    socket_init(socket);
    for (size_t i = 0; i < sizeof(file); i++) file[i] = (unsigned char)(i * 13 + 5);
    memcpy(file + 777, "\r\n------dio-boundary-7f3", 24);   /* near-miss inside the file */

    /* A 40 KB file through a 16 KB input buffer, in odd pieces: the sink
     * receives exactly the file, and the bridge gets message.attachment
     * with the staged path. */
    messageLength = build_upload(message, sizeof(message), "?guid=" TEST_PASSWORD, file, sizeof(file),
                                 TEMP_FIELD, NULL);
    CHECK(messageLength != 0);
    {
        size_t offset = 0;
        size_t piece = 3;
        while (offset < messageLength) {
            size_t take = piece < messageLength - offset ? piece : messageLength - offset;
            CHECK(bb_connection_receive(&socket->connection, message + offset, take));
            offset += take;
            piece = piece * 7 % 4093 + 1;
        }
    }
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStatePending);
    CHECK(harness->recordedCount == 1);
    CHECK(strcmp(last_recorded()->operation, "message.attachment") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON,
        "{\"chatGuid\":\"iMessage;-;+15555550100\",\"tempGuid\":\"temp-9\",\"name\":\"pic.jpg\","
        "\"contentType\":\"image/jpeg\",\"uploadPath\":\"/tmp/bbupload-fixture\",\"bytes\":40000}") == 0);
    CHECK(staged.opens == 1 && staged.closes == 1 && staged.kept && staged.deletes == 0);
    CHECK(strcmp(staged.filename, "pic.jpg") == 0 && strcmp(staged.contentType, "image/jpeg") == 0);
    CHECK(staged.length == sizeof(file) && memcmp(staged.bytes, file, sizeof(file)) == 0);
    CHECK(complete(last_recorded()->requestID, 200, "Attachment sent!",
                   "{\"guid\":\"sent-2\",\"tempGuid\":\"temp-9\",\"attachments\":[{\"guid\":\"a-1\"}]}", NULL, NULL));
    CHECK(output_has(socket, "HTTP/1.1 200 OK\r\n"));
    CHECK(output_has(socket, "\"message\":\"Attachment sent!\""));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);

    /* Rejections: the body is drained, the proper status answered, the
     * staged file discarded, and nothing reaches the bridge. */
    {
        struct { const char *query; const char *extra; const char *contentType; const char *expect; bool fileStaged; } cases[] = {
            { "?guid=" TEST_PASSWORD, "", NULL, "HTTP/1.1 400 ", true },                    /* no tempGuid */
            { "?guid=wrong", TEMP_FIELD, NULL, "HTTP/1.1 401 ", false },
            { "", TEMP_FIELD, NULL, "HTTP/1.1 401 ", false },
            { "?guid=" TEST_PASSWORD, TEMP_FIELD
              "\r\n------dio-boundary-7f3a\r\ncontent-disposition: form-data; name=\"effectId\"\r\n\r\nslam",
              NULL, "Message effects are not supported on iOS 9", true },
            { "?guid=" TEST_PASSWORD, TEMP_FIELD
              "\r\n------dio-boundary-7f3a\r\ncontent-disposition: form-data; name=\"subject\"\r\n\r\ns",
              NULL, "HTTP/1.1 400 ", true },
            { "?guid=" TEST_PASSWORD, TEMP_FIELD
              "\r\n------dio-boundary-7f3a\r\ncontent-disposition: form-data; name=\"selectedMessageGuid\"\r\n\r\ng",
              NULL, "Replies are not supported on iOS 9", true },
            { "?guid=" TEST_PASSWORD, TEMP_FIELD, "application/json", "Expected a multipart/form-data body", false },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            size_t before = harness->recordedCount;
            socket_init(socket);
            memset(&staged, 0, sizeof(staged));
            messageLength = build_upload(message, sizeof(message), cases[i].query, file, 100,
                                         cases[i].extra, cases[i].contentType);
            CHECK(bb_connection_receive(&socket->connection, message, messageLength));
            if (!output_has(socket, cases[i].expect)) {
                fprintf(stderr, "FAIL: upload case %zu lacks %s\n", i, cases[i].expect);
                failures++;
            }
            CHECK(harness->recordedCount == before);
            CHECK(!staged.open);
            CHECK(staged.opens == (cases[i].fileStaged ? 1U : 0U));
            CHECK(staged.deletes == staged.opens);
            CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
        }
    }

    /* Malformed multipart is refused outright: 400, close, file discarded. */
    socket_init(socket);
    memset(&staged, 0, sizeof(staged));
    messageLength = build_upload(message, sizeof(message), "?guid=" TEST_PASSWORD, file, 100, TEMP_FIELD, NULL);
    memcpy(message + messageLength - 4, "XX", 2);   /* garbage where the close "--" belongs */
    CHECK(!bb_connection_receive(&socket->connection, message, messageLength));
    CHECK(output_has(socket, "HTTP/1.1 400 ") && output_has(socket, "Upload refused"));
    CHECK(!staged.open && staged.deletes == 1);
    CHECK(bb_connection_wants_close(&socket->connection));

    /* A close delimiter that never matches (truncated boundary) is drained
     * and answered 400 without closing. */
    socket_init(socket);
    memset(&staged, 0, sizeof(staged));
    messageLength = build_upload(message, sizeof(message), "?guid=" TEST_PASSWORD, file, 100, TEMP_FIELD, NULL);
    memcpy(message + messageLength - 8, "XX", 2);
    CHECK(bb_connection_receive(&socket->connection, message, messageLength));
    CHECK(output_has(socket, "Malformed multipart body"));
    CHECK(!staged.open && staged.deletes == 1);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);

    /* The connection dropping mid-upload discards the file. */
    socket_init(socket);
    memset(&staged, 0, sizeof(staged));
    messageLength = build_upload(message, sizeof(message), "?guid=" TEST_PASSWORD, file, sizeof(file), TEMP_FIELD, NULL);
    CHECK(bb_connection_receive(&socket->connection, message, 2000));
    CHECK(staged.open && staged.length > 0);
    bb_connection_abort(&socket->connection);
    CHECK(!staged.open && staged.deletes == 1);

    /* A failing sink write answers 500 after draining. */
    socket_init(socket);
    memset(&staged, 0, sizeof(staged));
    staged.failWrite = true;
    messageLength = build_upload(message, sizeof(message), "?guid=" TEST_PASSWORD, file, 100, TEMP_FIELD, NULL);
    CHECK(bb_connection_receive(&socket->connection, message, messageLength));
    CHECK(output_has(socket, "HTTP/1.1 500 ") && output_has(socket, "Could not write the attachment"));
    CHECK(!staged.open && staged.deletes == 1);

    /* No sink configured: 501 after draining, nothing opened. */
    socket_init(socket);
    memset(&staged, 0, sizeof(staged));
    harness->config.uploads.openFile = NULL;
    messageLength = build_upload(message, sizeof(message), "?guid=" TEST_PASSWORD, file, 100, TEMP_FIELD, NULL);
    CHECK(bb_connection_receive(&socket->connection, message, messageLength));
    CHECK(output_has(socket, "HTTP/1.1 501 ") && staged.opens == 0);
    harness->config.uploads.openFile = staged_open;

    /* Other POSTs are not claimed by the upload path. */
    socket_init(socket);
    CHECK(feed_text(socket, "POST /api/v1/chat/query?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}"));
    CHECK(strcmp(last_recorded()->operation, "chat.query") == 0);
    CHECK(staged.opens == 0);
    free(socket);
}

static void test_attachment_download(void)
{
    Socket *socket = malloc(sizeof(*socket));
    uint64_t id;
    /* A JPEG-ish body with a NUL and high bytes, to prove raw (non-JSON) passthrough. */
    static const unsigned char body[] = { 0xFF, 0xD8, 0xFF, 0x00, 'h', 'i', 0x80, 0xFF, 0xD9 };
    harness_init();
    socket_init(socket);

    CHECK(feed_text(socket, "GET /api/v1/attachment/at_0_fixture-1/download?original=false&guid="
                            TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(socket->outputLength == 0);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStatePending);
    CHECK(harness->recordedCount == 1);
    CHECK(strcmp(last_recorded()->operation, "attachment.download") == 0);
    CHECK(strcmp(last_recorded()->argumentsJSON, "{\"guid\":\"at_0_fixture-1\"}") == 0);
    id = last_recorded()->requestID;

    /* The transport streams the file bytes as a raw body. */
    CHECK(bb_request_table_complete_raw(&harness->requestTable, id, 200, "image/jpeg",
                                        body, sizeof(body)));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(output_has(socket, "Content-Type: image/jpeg\r\n"));
    CHECK(output_has(socket, "Content-Length: 9\r\n"));
    CHECK(!output_has(socket, "\"status\""));
    {
        /* The body after the blank line is the exact raw bytes. */
        const char *bodyStart = strstr(output_text(socket), "\r\n\r\n") + 4;
        CHECK((size_t)(socket->output + socket->outputLength - (const unsigned char *)bodyStart) == sizeof(body));
        CHECK(memcmp(bodyStart, body, sizeof(body)) == 0);
    }
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
    CHECK(bb_request_table_active_count(&harness->requestTable) == 0);

    /* A missing/undownloaded attachment comes back as a normal 404 envelope
     * (the bridge sends errorJSON, not filePath). */
    reset_output(socket);
    CHECK(feed_text(socket, "GET /api/v1/attachment/nope/download?guid=" TEST_PASSWORD
                            " HTTP/1.1\r\n\r\n"));
    id = last_recorded()->requestID;
    CHECK(complete(id, 404, "Not Found", NULL, NULL,
                   "{\"type\":\"Database Error\",\"message\":\"Attachment has not been downloaded\"}"));
    CHECK(output_has(socket, "HTTP/1.1 404 Not Found\r\n"));
    CHECK(output_has(socket, "Attachment has not been downloaded"));

    /* complete_raw refuses a Socket.IO-origin request and unknown ids. */
    CHECK(!bb_request_table_complete_raw(&harness->requestTable, 999999, 200, "image/jpeg",
                                         body, sizeof(body)));
    free(socket);
}

static void test_table_primitives(void)
{
    BBRequestTable table;
    BBRequest slots[2];
    char scratch[512];
    BBEIOSession session;
    memset(&session, 0, sizeof(session));
    harness_init();
    CHECK(!bb_request_table_init(&table, slots, 2, 1, 0, NULL, NULL, scratch, sizeof(scratch)));
    CHECK(!bb_request_table_init(&table, slots, 2, 1, 0, fake_dispatch, harness, scratch, 100));
    CHECK(bb_request_table_init(&table, slots, 2, 0, 0, fake_dispatch, harness, scratch, sizeof(scratch)));
    CHECK(table.nextRequestID == 1 && table.timeoutMs == BB_REQUEST_DEFAULT_TIMEOUT_MS);
    /* Arguments must be a JSON object within bounds; operations must fit. */
    CHECK(bb_request_table_open_ack(&table, &session, 1, "chat.count", "[]", 2, 0) == NULL);
    CHECK(bb_request_table_open_ack(&table, &session, 1, "chat.count", "{", 1, 0) == NULL);
    CHECK(bb_request_table_open_ack(&table, &session, 1, "", "{}", 2, 0) == NULL);
    CHECK(bb_request_table_open_ack(&table, &session, 1,
        "this-operation-name-is-far-too-long", "{}", 2, 0) == NULL);
    CHECK(bb_request_table_open_ack(&table, NULL, 1, "chat.count", "{}", 2, 0) == NULL);
    CHECK(bb_request_table_open_http(&table, NULL, "chat.count", "{}", 2, 0) == NULL);
    {
        BBRequest *first = bb_request_table_open_ack(&table, &session, 7, "chat.count", "{}", 2, 10);
        BBRequest *second = bb_request_table_open_ack(&table, &session, 8, "chat.count", "{}", 2, 10);
        CHECK(first && second && first->requestID == 1 && second->requestID == 2);
        CHECK(bb_request_table_open_ack(&table, &session, 9, "chat.count", "{}", 2, 10) == NULL);
        CHECK(table.refused == 1);
        CHECK(bb_request_table_find(&table, 2) == second);
        CHECK(bb_request_table_dispatch(&table, first));
        CHECK(!bb_request_table_dispatch(&table, first));
        CHECK(bb_request_table_cancel_session(&table, &session) == 2);
        CHECK(bb_request_table_active_count(&table) == 0);
        CHECK(bb_request_table_find(&table, 1) == NULL);
    }
}

int main(void)
{
    harness = malloc(sizeof(*harness));
    test_chat_query_round_trip();
    test_out_of_order_completion();
    test_error_and_failure_replies();
    test_table_exhaustion_and_validation();
    test_socket_ack_round_trip();
    test_events_are_independent();
    test_message_send_route();
    test_chat_new_route();
    test_attachment_upload();
    test_attachment_download();
    test_table_primitives();
    free(harness);
    if (failures) return 1;
    puts("bridge tests passed: request correlation, out-of-order replies, ACKs, timeouts, cancellation, validation, events");
    return 0;
}
