#include "BBConnection.h"
#include "BBEngineIO.h"
#include "BBRouter.h"
#include "BBWebSocket.h"

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
#define SESSION_SLOTS 2
#define RS "\x1e"

/* One simulated TCP connection. */
typedef struct {
    BBConnection connection;
    unsigned char input[16384];
    char scratch[8192];
    unsigned char output[65536];
    size_t outputLength;
    BBEIOSession *session;      /* set when this socket upgraded */
    unsigned upgradedCalls;
    unsigned pendingCalls;
    bool alive;
} Socket;

/* Owner state shared by every connection: the session table and clock. */
typedef struct {
    BBEIOSession sessions[SESSION_SLOTS];
    unsigned char frames[SESSION_SLOTS][4096];
    unsigned char message[SESSION_SLOTS][4096];
    unsigned char outbound[SESSION_SLOTS][8192];
    Socket *socketForSlot[SESSION_SLOTS];
    BBEIOSessionTable table;
    BBRouterConfig config;
    unsigned sidCounter;
    uint64_t now;
    char eventScratch[4096];
    unsigned eventCalls;
    unsigned pollingReadyCalls;
    unsigned closedCalls;
    BBEIOCloseReason lastCloseReason;
    bool releaseOnClose;
} Harness;

static Harness *harness;

static size_t slot_index(const BBEIOSession *session)
{
    return (size_t)(session - harness->sessions);
}

static bool generate_sid(void *context, char *out, size_t capacity)
{
    Harness *owner = (Harness *)context;
    snprintf(out, capacity, "sid-%u", ++owner->sidCounter);
    return true;
}

static uint64_t clock_now(void *context)
{
    return ((Harness *)context)->now;
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
    owner->eventCalls++;
    CHECK(bb_router_socket_event(&owner->config, session, packet, packetLength,
                                 parsed, owner->eventScratch,
                                 sizeof(owner->eventScratch)));
}

/* Complete a held polling GET with whatever the session has queued. */
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
    ((Harness *)context)->pollingReadyCalls++;
    complete_polling_waiter(session);
}

static void session_closed(void *context, BBEIOSession *session,
                           BBEIOCloseReason reason)
{
    Harness *owner = (Harness *)context;
    owner->closedCalls++;
    owner->lastCloseReason = reason;
    if (session->transport == BBEIOTransportPolling) complete_polling_waiter(session);
    if (owner->releaseOnClose) bb_eio_table_release(session);
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
    (void)connection;
    return bb_router_route(&harness->config, connection, request, body, bodyLength,
                           scratch, scratchCapacity, response);
}

static void socket_upgraded(void *context, BBConnection *connection,
                            void *handlerContext)
{
    Socket *socket = (Socket *)context;
    BBEIOSession *session = (BBEIOSession *)handlerContext;
    (void)connection;
    socket->upgradedCalls++;
    socket->session = session;
    harness->socketForSlot[slot_index(session)] = socket;
    CHECK(bb_eio_session_start(session));
}

static void socket_upgraded_data(void *context, BBConnection *connection,
                                 const unsigned char *bytes, size_t length)
{
    Socket *socket = (Socket *)context;
    (void)connection;
    if (!socket->session) return;
    (void)bb_eio_session_receive_bytes(socket->session, bytes, length, harness->now);
}

static void socket_pending(void *context, BBConnection *connection,
                           void *handlerContext)
{
    Socket *socket = (Socket *)context;
    BBEIOSession *session = (BBEIOSession *)handlerContext;
    (void)connection;
    socket->pendingCalls++;
    session->pollingWaiter = socket;
}

static void socket_init(Socket *socket)
{
    BBConnectionCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    memset(socket, 0, sizeof(*socket));
    callbacks.handleRequest = socket_handle;
    callbacks.emit = socket_emit;
    callbacks.upgradedData = socket_upgraded_data;
    callbacks.upgraded = socket_upgraded;
    callbacks.pending = socket_pending;
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
    harness->now = 1000000;
    harness->releaseOnClose = true;

    harness->config.password = TEST_PASSWORD;
    harness->config.metadata.computerID = "bluebubbles-ios-host-test";
    harness->config.metadata.osVersion = "9.3.5";
    harness->config.metadata.serverVersion = "0.2.0";
    harness->config.metadata.proxyService = "Direct";
    harness->config.sessions = &harness->table;
    harness->config.sessionCallbacks.event = session_event;
    harness->config.sessionCallbacks.sendFrame = session_send_frame;
    harness->config.sessionCallbacks.pollingReady = session_polling_ready;
    harness->config.sessionCallbacks.closed = session_closed;
    harness->config.sessionCallbacks.context = harness;
    harness->config.nowMs = clock_now;
    harness->config.clockContext = harness;
}

static bool feed(Socket *socket, const void *bytes, size_t length)
{
    socket->alive = bb_connection_receive(&socket->connection, bytes, length);
    return socket->alive;
}

static bool feed_text(Socket *socket, const char *text)
{
    return feed(socket, text, strlen(text));
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

/* Build a masked client frame. */
static size_t client_frame(unsigned char *out, unsigned char opcode, bool fin,
                           const unsigned char *payload, size_t length)
{
    static const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    size_t cursor = 0;
    out[cursor++] = (unsigned char)((fin ? 0x80 : 0x00) | opcode);
    if (length < 126) {
        out[cursor++] = (unsigned char)(0x80 | length);
    } else {
        out[cursor++] = 0x80 | 126;
        out[cursor++] = (unsigned char)(length >> 8);
        out[cursor++] = (unsigned char)length;
    }
    memcpy(out + cursor, mask, 4);
    cursor += 4;
    for (size_t i = 0; i < length; i++) out[cursor++] = (unsigned char)(payload[i] ^ mask[i % 4]);
    return cursor;
}

static bool send_text_frame(Socket *socket, const char *text)
{
    unsigned char frame[4096];
    size_t length = client_frame(frame, 0x1, true, (const unsigned char *)text,
                                 strlen(text));
    return feed(socket, frame, length);
}

/* Send text as an initial fragment plus one continuation. */
static bool send_fragmented_text(Socket *socket, const char *text, size_t split)
{
    unsigned char frame[4096];
    size_t length = strlen(text);
    size_t frameLength = client_frame(frame, 0x1, false, (const unsigned char *)text, split);
    if (!feed(socket, frame, frameLength)) return false;
    frameLength = client_frame(frame, 0x0, true, (const unsigned char *)text + split,
                               length - split);
    return feed(socket, frame, frameLength);
}

/* Parse server frames from the socket output; returns the count and copies
 * the payload of frame `index` (unmasked server frames). */
static size_t server_frames(Socket *socket, size_t index, unsigned char *opcode,
                            char *payload, size_t payloadCapacity)
{
    size_t cursor = 0;
    size_t count = 0;
    while (cursor + 2 <= socket->outputLength) {
        unsigned char first = socket->output[cursor];
        unsigned char lengthCode = (unsigned char)(socket->output[cursor + 1] & 0x7f);
        size_t headerLength = 2;
        size_t length = lengthCode;
        CHECK((socket->output[cursor + 1] & 0x80) == 0);
        if (lengthCode == 126) {
            length = ((size_t)socket->output[cursor + 2] << 8) | socket->output[cursor + 3];
            headerLength = 4;
        } else if (lengthCode == 127) {
            length = 0;
            for (size_t i = 0; i < 8; i++) length = (length << 8) | socket->output[cursor + 2 + i];
            headerLength = 10;
        }
        if (cursor + headerLength + length > socket->outputLength) break;
        if (count == index) {
            if (opcode) *opcode = (unsigned char)(first & 0x0f);
            if (payload && length < payloadCapacity) {
                memcpy(payload, socket->output + cursor + headerLength, length);
                payload[length] = '\0';
            }
        }
        cursor += headerLength + length;
        count++;
    }
    return count;
}

static const char handshake[] =
    "GET /socket.io/?EIO=4&transport=websocket&guid=" TEST_PASSWORD " HTTP/1.1\r\n"
    "Host: fixture\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";

static void test_websocket_transcript(void)
{
    Socket *socket = malloc(sizeof(*socket));
    unsigned char opcode = 0;
    char payload[4096];
    harness_init();
    socket_init(socket);

    /* Trailing frame bytes arrive with the handshake: fragmented "40". */
    unsigned char bytes[600];
    size_t length = strlen(handshake);
    memcpy(bytes, handshake, length);
    length += client_frame(bytes + length, 0x1, false, (const unsigned char *)"4", 1);
    length += client_frame(bytes + length, 0x0, true, (const unsigned char *)"0", 1);
    CHECK(feed(socket, bytes, length));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 101 Switching Protocols\r\n", 34) == 0);
    CHECK(output_has(socket, "Upgrade: websocket\r\n"));
    CHECK(output_has(socket, "Connection: Upgrade\r\n"));
    CHECK(output_has(socket, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"));
    CHECK(socket->upgradedCalls == 1 && socket->session != NULL);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateUpgraded);
    CHECK(bb_eio_table_active_count(&harness->table) == 1);

    /* Everything after the HTTP head is framed. */
    const char *frameStart = strstr(output_text(socket), "\r\n\r\n") + 4;
    size_t headLength = (size_t)(frameStart - output_text(socket));
    memmove(socket->output, socket->output + headLength, socket->outputLength - headLength);
    socket->outputLength -= headLength;
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 2);
    server_frames(socket, 0, &opcode, payload, sizeof(payload));
    CHECK(opcode == 0x1);
    CHECK(strcmp(payload,
        "0{\"sid\":\"sid-1\",\"upgrades\":[],\"pingInterval\":60000,"
        "\"pingTimeout\":120000,\"maxPayload\":1000000}") == 0);
    server_frames(socket, 1, &opcode, payload, sizeof(payload));
    CHECK(strcmp(payload, "40{\"sid\":\"sid-2\"}") == 0);
    CHECK(socket->session->state == BBEIOSessionStateConnected);

    /* Event with ACK id 731, fragmented at byte 17. */
    reset_output(socket);
    CHECK(send_fragmented_text(socket, "42731[\"get-server-metadata\",{}]", 17));
    CHECK(harness->eventCalls == 1);
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(strncmp(payload, "43731[{\"status\":200,\"message\":\"Successfully fetched metadata\","
                           "\"data\":{\"computer_id\":\"bluebubbles-ios-host-test\"", 96) == 0);
    CHECK(strstr(payload, "\"server_version\":\"0.2.0\"") != NULL);
    CHECK(payload[strlen(payload) - 1] == ']');
    CHECK(socket->session->acksSent == 1);

    /* Unsupported event with ACK gets a capability error; without ACK, nothing. */
    reset_output(socket);
    CHECK(send_text_frame(socket, "4212[\"send-reaction\",{\"reaction\":\"love\"}]"));
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(strcmp(payload, "4312[{\"status\":400,\"message\":\"Bad Request\",\"error\":"
                          "{\"type\":\"Validation Error\",\"message\":\"Unsupported event\"}}]") == 0);
    /* Bridge-backed events without a request table answer 503. */
    reset_output(socket);
    CHECK(send_text_frame(socket, "4213[\"get-chats\",{\"limit\":1}]"));
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(strncmp(payload, "4313[{\"status\":503,", 19) == 0);
    CHECK(strstr(payload, "The messaging bridge is unavailable") != NULL);
    reset_output(socket);
    CHECK(send_text_frame(socket, "42[\"started-typing\",{\"chatGuid\":\"x\"}]"));
    CHECK(socket->outputLength == 0 && harness->eventCalls == 4);

    /* Large event spanning a 16-bit length frame. */
    {
        static char big[3000];
        memcpy(big, "4277[\"get-server-metadata\",{\"pad\":\"", 36);
        memset(big + 36, 'p', 2900);
        memcpy(big + 2936, "\"}]", 4);
        reset_output(socket);
        CHECK(send_text_frame(socket, big));
        CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
        CHECK(strncmp(payload, "4377[{\"status\":200", 18) == 0);
    }

    /* WebSocket ping is answered with a pong carrying the same payload. */
    reset_output(socket);
    {
        unsigned char frame[64];
        size_t frameLength = client_frame(frame, 0x9, true, (const unsigned char *)"hi", 2);
        CHECK(feed(socket, frame, frameLength));
    }
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(opcode == 0xA && strcmp(payload, "hi") == 0);

    /* Heartbeat is driven by the caller's clock. */
    reset_output(socket);
    bb_eio_session_tick(socket->session, harness->now + 59999);
    CHECK(socket->outputLength == 0);
    bb_eio_session_tick(socket->session, harness->now + 60000);
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(strcmp(payload, "2") == 0);
    bb_eio_session_tick(socket->session, harness->now + 60001);
    CHECK(server_frames(socket, 0, NULL, NULL, 0) == 1);
    harness->now += 60500;
    CHECK(send_text_frame(socket, "3"));
    CHECK(!socket->session->awaitingPong && socket->session->pongsReceived == 1);
    reset_output(socket);

    /* Server push events carry raw objects, not envelopes. */
    CHECK(bb_eio_session_emit(socket->session, "new-message",
                              "{\"guid\":\"fixture-message-1\",\"chats\":[]}", 39));
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(strcmp(payload, "42[\"new-message\",{\"guid\":\"fixture-message-1\",\"chats\":[]}]") == 0);

    /* Missing pong closes the session after pingTimeout. */
    reset_output(socket);
    bb_eio_session_tick(socket->session, harness->now + 60000);
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(strcmp(payload, "2") == 0);
    bb_eio_session_tick(socket->session, harness->now + 60000 + 119999);
    CHECK(harness->closedCalls == 0);
    bb_eio_session_tick(socket->session, harness->now + 60000 + 120000);
    CHECK(harness->closedCalls == 1 && harness->lastCloseReason == BBEIOClosePingTimeout);
    CHECK(server_frames(socket, 1, &opcode, payload, sizeof(payload)) == 2);
    CHECK(opcode == 0x8);
    CHECK(socket->session->state == BBEIOSessionStateUnused);
    CHECK(bb_eio_table_active_count(&harness->table) == 0);
    free(socket);
}

static Socket *open_websocket(void)
{
    Socket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    CHECK(feed_text(socket, handshake));
    CHECK(socket->session != NULL);
    reset_output(socket);
    return socket;
}

static void test_websocket_close_paths(void)
{
    Socket *socket;
    unsigned char opcode = 0;
    char payload[256];
    unsigned char frame[64];
    size_t frameLength;

    /* Peer close frame: one close frame back, PeerFrame reason. */
    harness_init();
    socket = open_websocket();
    CHECK(send_text_frame(socket, "40"));
    reset_output(socket);
    frameLength = client_frame(frame, 0x8, true, (const unsigned char *)"\x03\xe8", 2);
    CHECK(feed(socket, frame, frameLength));
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(opcode == 0x8);
    CHECK(harness->closedCalls == 1 && harness->lastCloseReason == BBEIOClosePeerFrame);
    free(socket);

    /* Socket.IO disconnect. */
    harness_init();
    socket = open_websocket();
    CHECK(send_text_frame(socket, "40"));
    CHECK(send_text_frame(socket, "41"));
    CHECK(harness->closedCalls == 1 && harness->lastCloseReason == BBEIOClosePeerDisconnect);
    free(socket);

    /* Engine.IO close packet. */
    harness_init();
    socket = open_websocket();
    CHECK(send_text_frame(socket, "1"));
    CHECK(harness->lastCloseReason == BBEIOClosePeerDisconnect);
    free(socket);

    /* Protocol errors: binary frame, stray continuation, text during a
     * fragmented message, event before connect, malformed packet. */
    harness_init();
    socket = open_websocket();
    frameLength = client_frame(frame, 0x2, true, (const unsigned char *)"\x01", 1);
    CHECK(feed(socket, frame, frameLength));
    CHECK(harness->lastCloseReason == BBEIOCloseProtocolError);
    free(socket);

    harness_init();
    socket = open_websocket();
    frameLength = client_frame(frame, 0x0, true, (const unsigned char *)"0", 1);
    CHECK(feed(socket, frame, frameLength));
    CHECK(harness->lastCloseReason == BBEIOCloseProtocolError);
    free(socket);

    harness_init();
    socket = open_websocket();
    frameLength = client_frame(frame, 0x1, false, (const unsigned char *)"4", 1);
    CHECK(feed(socket, frame, frameLength));
    CHECK(harness->closedCalls == 0);
    CHECK(send_text_frame(socket, "40"));
    CHECK(harness->lastCloseReason == BBEIOCloseProtocolError);
    free(socket);

    harness_init();
    socket = open_websocket();
    CHECK(send_text_frame(socket, "42[\"early\",{}]"));
    CHECK(harness->lastCloseReason == BBEIOCloseProtocolError);
    free(socket);

    harness_init();
    socket = open_websocket();
    CHECK(send_text_frame(socket, "4"));
    CHECK(harness->lastCloseReason == BBEIOCloseProtocolError);
    free(socket);

    /* Invalid namespace is rejected without closing. */
    harness_init();
    socket = open_websocket();
    CHECK(send_text_frame(socket, "40/admin,"));
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(strcmp(payload, "44{\"message\":\"Invalid namespace\"}") == 0);
    CHECK(socket->session->state == BBEIOSessionStateOpening);
    reset_output(socket);
    CHECK(send_text_frame(socket, "40/,"));
    CHECK(server_frames(socket, 0, &opcode, payload, sizeof(payload)) == 1);
    CHECK(strcmp(payload, "40{\"sid\":\"sid-2\"}") == 0);
    CHECK(socket->session->state == BBEIOSessionStateConnected);
    free(socket);

    /* Emit while the socket refuses writes closes the session. */
    harness_init();
    socket = open_websocket();
    CHECK(send_text_frame(socket, "40"));
    socket->outputLength = sizeof(socket->output);
    CHECK(!bb_eio_session_emit(socket->session, "hello-world", "null", 4));
    CHECK(harness->lastCloseReason == BBEIOCloseTransportFailed);
    free(socket);
}

static void test_websocket_handshake_rejections(void)
{
    Socket *socket = malloc(sizeof(*socket));
    Socket *second = malloc(sizeof(*second));
    Socket *third = malloc(sizeof(*third));
    harness_init();
    socket_init(socket);

    CHECK(feed_text(socket,
        "GET /socket.io/?EIO=3&transport=websocket&guid=" TEST_PASSWORD " HTTP/1.1\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 400 ") && output_has(socket, "Engine.IO v4 required"));
    reset_output(socket);
    CHECK(feed_text(socket,
        "GET /socket.io/?EIO=4&transport=websocket&guid=" TEST_PASSWORD " HTTP/1.1\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 400 ") && output_has(socket, "RFC 6455"));
    reset_output(socket);
    CHECK(feed_text(socket,
        "GET /socket.io/?EIO=4&transport=websocket&guid=" TEST_PASSWORD " HTTP/1.1\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 8\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 400 "));
    reset_output(socket);
    CHECK(feed_text(socket,
        "GET /socket.io/?EIO=4&transport=flashsocket&guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "Unsupported Engine.IO transport"));
    reset_output(socket);
    CHECK(feed_text(socket, "GET /socket.io/?EIO=4&transport=websocket HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 401 "));
    CHECK(bb_eio_table_active_count(&harness->table) == 0);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);

    /* Table exhaustion answers 503 and leaves the connection in HTTP. */
    CHECK(feed_text(socket, handshake));
    socket_init(second);
    CHECK(feed_text(second, handshake));
    CHECK(bb_eio_table_active_count(&harness->table) == 2);
    socket_init(third);
    CHECK(feed_text(third, handshake));
    CHECK(output_has(third, "HTTP/1.1 503 Service Unavailable\r\n"));
    CHECK(output_has(third, "\"type\":\"Socket Error\""));
    CHECK(bb_connection_state(&third->connection) == BBConnectionStateHTTP);

    /* Closing one frees its slot for the next. */
    CHECK(send_text_frame(socket, "1"));
    reset_output(third);
    CHECK(feed_text(third, handshake));
    CHECK(output_has(third, "HTTP/1.1 101 "));

    /* With no session table the route is a plain 404. */
    harness->config.sessions = NULL;
    socket_init(socket);
    CHECK(feed_text(socket, handshake));
    CHECK(output_has(socket, "HTTP/1.1 404 Not Found\r\n"));
    free(socket);
    free(second);
    free(third);
}

static void test_polling_transcript(void)
{
    Socket *get = malloc(sizeof(*get));
    Socket *post = malloc(sizeof(*post));
    BBEIOSession *session;
    harness_init();
    socket_init(get);
    socket_init(post);

    CHECK(feed_text(get, "GET /socket.io/?EIO=4&transport=polling&guid=" TEST_PASSWORD
                         " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(get, "HTTP/1.1 200 OK\r\n"));
    CHECK(output_has(get, "Content-Type: text/plain; charset=utf-8\r\n"));
    CHECK(output_has(get, "\r\n\r\n0{\"sid\":\"sid-1\",\"upgrades\":[],\"pingInterval\":60000,"
                          "\"pingTimeout\":120000,\"maxPayload\":1000000}"));
    session = bb_eio_table_find(&harness->table, "sid-1");
    CHECK(session && session->transport == BBEIOTransportPolling);
    CHECK(bb_connection_state(&get->connection) == BBConnectionStateHTTP);

    /* Namespace connect through POST, answered on the next GET. */
    reset_output(post);
    CHECK(feed_text(post, "POST /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 2\r\n\r\n40"));
    CHECK(output_has(post, "HTTP/1.1 200 OK\r\n") && output_has(post, "\r\n\r\nok"));
    CHECK(harness->pollingReadyCalls == 1);
    reset_output(get);
    CHECK(feed_text(get, "GET /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                         " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(get, "\r\n\r\n40{\"sid\":\"sid-2\"}"));
    CHECK(bb_eio_session_queued_bytes(session) == 0);

    /* Empty queue: the GET is held until something is queued. */
    reset_output(get);
    CHECK(feed_text(get, "GET /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                         " HTTP/1.1\r\n\r\n"));
    CHECK(get->outputLength == 0);
    CHECK(bb_connection_state(&get->connection) == BBConnectionStatePending);
    CHECK(get->pendingCalls == 1 && session->pollingWaiter == get);
    /* A pipelined request arriving meanwhile is buffered, not served. */
    CHECK(feed_text(get, "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(get->outputLength == 0);
    /* The heartbeat releases it. */
    bb_eio_session_tick(session, harness->now + 60000);
    CHECK(output_has(get, "HTTP/1.1 200 OK\r\n") && output_has(get, "\r\n\r\n2HTTP/1.1 200 OK"));
    CHECK(output_has(get, "\"data\":\"pong\""));
    CHECK(bb_connection_state(&get->connection) == BBConnectionStateHTTP);
    CHECK(session->pollingWaiter == NULL);

    /* Pong plus an ACK event in one record-separated POST body. */
    reset_output(post);
    CHECK(feed_text(post, "POST /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 32\r\n\r\n"
                          "3" RS "4219[\"get-server-metadata\",{}]"));
    CHECK(output_has(post, "\r\n\r\nok"));
    CHECK(!session->awaitingPong && harness->eventCalls == 1);
    /* Queue a push as well, then read both packets in one GET. */
    CHECK(bb_eio_session_emit(session, "updated-message", "{\"guid\":\"m2\"}", 13));
    reset_output(get);
    CHECK(feed_text(get, "GET /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                         " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(get, "\r\n\r\n4319[{\"status\":200,\"message\":\"Successfully fetched metadata\""));
    CHECK(output_has(get, "}}]" RS "42[\"updated-message\",{\"guid\":\"m2\"}]"));

    /* Error paths. */
    reset_output(post);
    CHECK(feed_text(post, "POST /socket.io/?EIO=4&transport=polling&sid=nope&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 2\r\n\r\n40"));
    CHECK(output_has(post, "HTTP/1.1 400 ") && output_has(post, "Unknown Engine.IO sid"));
    reset_output(post);
    CHECK(feed_text(post, "POST /socket.io/?EIO=4&transport=polling&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 2\r\n\r\n40"));
    CHECK(output_has(post, "Initial polling request must be GET"));
    reset_output(post);
    CHECK(feed_text(post, "PUT /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 0\r\n\r\n"));
    CHECK(output_has(post, "HTTP/1.1 405 "));

    /* A malformed payload closes the session; the next GET reads "1". */
    harness->releaseOnClose = false;
    reset_output(post);
    CHECK(feed_text(post, "POST /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 3\r\n\r\n42x"));
    CHECK(output_has(post, "HTTP/1.1 400 ") && output_has(post, "Malformed Engine.IO payload"));
    CHECK(harness->closedCalls == 1 && harness->lastCloseReason == BBEIOCloseProtocolError);
    CHECK(session->state == BBEIOSessionStateClosed);
    reset_output(get);
    CHECK(feed_text(get, "GET /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                         " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(get, "\r\n\r\n1"));
    bb_eio_table_release(session);
    CHECK(bb_eio_table_active_count(&harness->table) == 0);
    free(get);
    free(post);
}

static void test_polling_close_while_held(void)
{
    Socket *get = malloc(sizeof(*get));
    Socket *post = malloc(sizeof(*post));
    BBEIOSession *session;
    harness_init();
    socket_init(get);
    socket_init(post);
    CHECK(feed_text(get, "GET /socket.io/?EIO=4&transport=polling&guid=" TEST_PASSWORD
                         " HTTP/1.1\r\n\r\n"));
    session = bb_eio_table_find(&harness->table, "sid-1");
    CHECK(session != NULL);
    CHECK(feed_text(post, "POST /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 2\r\n\r\n40"));
    reset_output(get);
    CHECK(feed_text(get, "GET /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                         " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(get, "40{\"sid\":\"sid-2\"}"));
    reset_output(get);
    CHECK(feed_text(get, "GET /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                         " HTTP/1.1\r\n\r\n"));
    CHECK(bb_connection_state(&get->connection) == BBConnectionStatePending);

    /* The peer disconnects while the GET is held: it completes with "1". */
    reset_output(post);
    CHECK(feed_text(post, "POST /socket.io/?EIO=4&transport=polling&sid=sid-1&guid=" TEST_PASSWORD
                          " HTTP/1.1\r\nContent-Length: 2\r\n\r\n41"));
    CHECK(output_has(post, "\r\n\r\nok"));
    CHECK(harness->lastCloseReason == BBEIOClosePeerDisconnect);
    CHECK(output_has(get, "\r\n\r\n1"));
    CHECK(bb_connection_state(&get->connection) == BBConnectionStateHTTP);
    CHECK(bb_eio_table_active_count(&harness->table) == 0);

    /* A websocket session id is not usable through polling. */
    {
        Socket *ws = open_websocket();
        reset_output(post);
        CHECK(feed_text(post, "GET /socket.io/?EIO=4&transport=polling&sid="
                              "sid-3&guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
        CHECK(output_has(post, "Unknown Engine.IO sid"));
        free(ws);
    }
    free(get);
    free(post);
}

static void test_session_primitives(void)
{
    BBEIOSession session;
    BBEIOCallbacks callbacks;
    static unsigned char outbound[64];
    char packet[BB_EIO_OPEN_PACKET_BYTES];
    size_t length = 0;
    harness_init();
    memset(&session, 0, sizeof(session));
    callbacks = harness->config.sessionCallbacks;

    CHECK(bb_eio_write_open_packet("abc_-1", packet, sizeof(packet), &length));
    CHECK(length == strlen(packet));
    CHECK(!bb_eio_write_open_packet("bad sid", packet, sizeof(packet), &length));
    CHECK(!bb_eio_write_open_packet("", packet, sizeof(packet), &length));
    CHECK(!bb_eio_write_open_packet("x", packet, 20, &length));

    /* Buffers are required per transport. */
    CHECK(!bb_eio_session_init(&session, BBEIOTransportWebSocket, "a", "b", &callbacks, 0));
    bb_eio_session_set_buffers(&session, NULL, 0, NULL, 0, harness->outbound[0],
                               sizeof(harness->outbound[0]));
    CHECK(!bb_eio_session_init(&session, BBEIOTransportWebSocket, "a", "b", &callbacks, 0));
    CHECK(bb_eio_session_init(&session, BBEIOTransportPolling, "a", "b", &callbacks, 0));
    CHECK(session.state == BBEIOSessionStateOpening);
    CHECK(!bb_eio_session_init(&session, BBEIOTransportPolling, "a", "a b", &callbacks, 0));
    bb_eio_session_set_buffers(&session, NULL, 0, NULL, 0, outbound, sizeof(outbound));
    CHECK(!bb_eio_session_init(&session, BBEIOTransportPolling, "a", "b", &callbacks, 0));

    /* Polling queue exhaustion closes the session. */
    bb_eio_session_set_buffers(&session, NULL, 0, NULL, 0, harness->outbound[0], 300);
    CHECK(bb_eio_session_init(&session, BBEIOTransportPolling, "a", "b", &callbacks, 0));
    harness->releaseOnClose = false;
    for (int i = 0; i < 6 && session.state != BBEIOSessionStateClosed; i++) {
        static const char filler[] =
            "42[\"x\",\"0123456789012345678901234567890123456789012345678901234567890123456789\"]";
        (void)bb_eio_session_send_packet(&session, (const unsigned char *)filler,
                                         sizeof(filler) - 1);
    }
    CHECK(session.state == BBEIOSessionStateClosed);
    CHECK(harness->lastCloseReason == BBEIOCloseBufferExhausted);
    CHECK(bb_eio_session_queued_bytes(&session) == 1 && harness->outbound[0][0] == '1');

    /* Table sid generation failures and duplicates are refused. */
    harness_init();
    harness->table.generateSid = NULL;
    CHECK(bb_eio_table_take(&harness->table, BBEIOTransportPolling, &callbacks, 0) == NULL);
    CHECK(bb_eio_table_find(&harness->table, "sid-1") == NULL);
    CHECK(bb_eio_table_find(&harness->table, NULL) == NULL);
}

int main(void)
{
    harness = malloc(sizeof(*harness));
    test_websocket_transcript();
    test_websocket_close_paths();
    test_websocket_handshake_rejections();
    test_polling_transcript();
    test_polling_close_while_held();
    test_session_primitives();
    free(harness);
    if (failures) return 1;
    puts("engine.io tests passed: websocket handshake, fragments, ACKs, heartbeat, pushes, polling hold/drain, close paths");
    return 0;
}
