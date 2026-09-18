#include "BBConnection.h"
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

/* Fictional password used only by this host test. */
#define TEST_PASSWORD "fixture-only"

#define INPUT_CAPACITY 8192
#define SCRATCH_CAPACITY 4096
#define OUTPUT_CAPACITY 16384

/* Everything captured for one simulated socket. */
typedef struct {
    BBConnection connection;
    unsigned char input[INPUT_CAPACITY];
    char scratch[SCRATCH_CAPACITY];
    unsigned char output[OUTPUT_CAPACITY];
    size_t outputLength;
    unsigned emitCalls;
    bool failEmit;
    unsigned char upgraded[1024];
    size_t upgradedLength;
    unsigned handlerCalls;
    char lastBody[512];
    size_t lastBodyLength;
    char lastPath[128];
    BBRouterConfig config;
} TestSocket;

static bool test_emit(void *context, BBConnection *connection,
                      const unsigned char *bytes, size_t length)
{
    TestSocket *socket = (TestSocket *)context;
    CHECK(connection == &socket->connection);
    socket->emitCalls++;
    if (socket->failEmit) return false;
    if (length > sizeof(socket->output) - socket->outputLength) return false;
    memcpy(socket->output + socket->outputLength, bytes, length);
    socket->outputLength += length;
    return true;
}

static void test_upgraded(void *context, BBConnection *connection,
                          const unsigned char *bytes, size_t length)
{
    TestSocket *socket = (TestSocket *)context;
    (void)connection;
    if (length > sizeof(socket->upgraded) - socket->upgradedLength) return;
    memcpy(socket->upgraded + socket->upgradedLength, bytes, length);
    socket->upgradedLength += length;
}

/* Test-only routes sit in front of the real router: /echo returns the
 * request body, /socket.io/ upgrades, /fail simulates a handler failure. */
static bool test_handle(void *context, BBConnection *connection,
                        const BBHTTPRequest *request,
                        const unsigned char *body, size_t bodyLength,
                        char *scratch, size_t scratchCapacity,
                        BBHTTPResponse *response)
{
    TestSocket *socket = (TestSocket *)context;
    CHECK(connection == &socket->connection);
    socket->handlerCalls++;
    socket->lastBodyLength = bodyLength < sizeof(socket->lastBody) ?
        bodyLength : sizeof(socket->lastBody) - 1;
    memcpy(socket->lastBody, body, socket->lastBodyLength);
    socket->lastBody[socket->lastBodyLength] = '\0';
    strncpy(socket->lastPath, request->path, sizeof(socket->lastPath) - 1);
    socket->lastPath[sizeof(socket->lastPath) - 1] = '\0';

    if (strcmp(request->path, "/echo") == 0) {
        bb_response_init(response);
        response->contentType = BB_RESPONSE_CONTENT_TYPE_TEXT;
        response->body = body;
        response->bodyLength = bodyLength;
        return true;
    }
    if (strcmp(request->path, "/fail") == 0) return false;
    if (strcmp(request->path, "/pending") == 0) {
        bb_response_init(response);
        response->pending = true;
        response->handlerContext = socket;
        return true;
    }
    if (strcmp(request->path, "/force-upgrade") == 0) {
        /* Misbehaving handler: 101 on a request that never asked to upgrade. */
        bb_response_init(response);
        response->status = 101;
        response->extraHeaders = "Upgrade: websocket\r\n";
        return true;
    }
    if (strcmp(request->path, "/socket.io/") == 0 && request->upgradeWebSocket) {
        bb_response_init(response);
        response->status = 101;
        response->cors = false;
        response->extraHeaders = "Upgrade: websocket\r\n"
                                 "Sec-WebSocket-Accept: fixture-accept\r\n";
        return true;
    }
    return bb_router_route(&socket->config, connection, request, body, bodyLength,
                           scratch, scratchCapacity, response);
}

static unsigned pendingCallbacks;
static void *lastPendingContext;

static void test_pending(void *context, BBConnection *connection,
                         void *handlerContext)
{
    (void)context;
    (void)connection;
    pendingCallbacks++;
    lastPendingContext = handlerContext;
}

static void socket_init(TestSocket *socket)
{
    BBConnectionCallbacks callbacks;
    memset(socket, 0, sizeof(*socket));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.handleRequest = test_handle;
    callbacks.emit = test_emit;
    callbacks.upgradedData = test_upgraded;
    callbacks.pending = test_pending;
    callbacks.context = socket;
    socket->config.password = TEST_PASSWORD;
    socket->config.metadata.computerID = "bluebubbles-ios-host-test";
    socket->config.metadata.osVersion = "9.3.5";
    socket->config.metadata.serverVersion = "0.2.0";
    socket->config.metadata.proxyService = "Direct";
    socket->config.metadata.localIPv4[0] = "127.0.0.1";
    socket->config.metadata.localIPv4Count = 1;
    CHECK(bb_connection_init(&socket->connection, socket->input,
                             sizeof(socket->input), socket->scratch,
                             sizeof(socket->scratch), &callbacks));
}

static bool feed(TestSocket *socket, const char *text)
{
    return bb_connection_receive(&socket->connection,
                                 (const unsigned char *)text, strlen(text));
}

/* Feed one byte at a time and report whether every call returned true. */
static bool feed_bytewise(TestSocket *socket, const char *text)
{
    bool ok = true;
    for (size_t i = 0; text[i]; i++) {
        ok = bb_connection_receive(&socket->connection,
                                   (const unsigned char *)text + i, 1) && ok;
    }
    return ok;
}

static const char *output_text(TestSocket *socket)
{
    socket->output[socket->outputLength] = '\0';
    return (const char *)socket->output;
}

static bool output_has(TestSocket *socket, const char *needle)
{
    return strstr(output_text(socket), needle) != NULL;
}

static size_t output_count(TestSocket *socket, const char *needle)
{
    const char *cursor = output_text(socket);
    size_t count = 0;
    size_t needleLength = strlen(needle);
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needleLength;
    }
    return count;
}

static void reset_output(TestSocket *socket)
{
    socket->outputLength = 0;
    socket->emitCalls = 0;
}

/* Verify that the declared Content-Length matches the emitted body. */
static bool response_framing_consistent(TestSocket *socket)
{
    const char *text = output_text(socket);
    const char *headerEnd = strstr(text, "\r\n\r\n");
    const char *lengthHeader = strstr(text, "Content-Length: ");
    if (!headerEnd || !lengthHeader || lengthHeader > headerEnd) return false;
    size_t declared = (size_t)strtoul(lengthHeader + 16, NULL, 10);
    size_t headLength = (size_t)(headerEnd + 4 - text);
    return socket->outputLength == headLength + declared;
}

static void test_ping_fragmented_headers(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    const char request[] =
        "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
        "Host: fixture\r\nUser-Agent: host-test\r\n\r\n";
    size_t partial = strlen(request) - 1;
    CHECK(bb_connection_receive(&socket->connection,
                                (const unsigned char *)request, partial));
    CHECK(socket->outputLength == 0 && socket->handlerCalls == 0);
    CHECK(socket->connection.inputLength == partial);
    CHECK(feed(socket, "\n"));
    CHECK(socket->handlerCalls == 1);
    CHECK(strncmp(output_text(socket), "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(output_has(socket, "Content-Type: application/json; charset=utf-8\r\n"));
    CHECK(output_has(socket, "Connection: keep-alive\r\n"));
    CHECK(output_has(socket, "Access-Control-Allow-Origin: *\r\n"));
    CHECK(output_has(socket,
        "\r\n\r\n{\"status\":200,\"message\":\"Ping received!\",\"data\":\"pong\"}"));
    CHECK(response_framing_consistent(socket));
    CHECK(socket->connection.inputLength == 0);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
    CHECK(!bb_connection_wants_close(&socket->connection));
    CHECK(socket->connection.requestsServed == 1);

    /* Byte-at-a-time delivery must behave identically. */
    reset_output(socket);
    CHECK(feed_bytewise(socket, request));
    CHECK(socket->handlerCalls == 2);
    CHECK(output_has(socket, "\"data\":\"pong\"}"));
    CHECK(response_framing_consistent(socket));
    free(socket);
}

static void test_fragmented_body_and_echo(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    const char head[] =
        "POST /echo?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
        "Host: fixture\r\nContent-Type: application/json\r\n"
        "Content-Length: 25\r\n\r\n";
    CHECK(feed(socket, head));
    CHECK(socket->handlerCalls == 0 && socket->outputLength == 0);
    CHECK(feed(socket, "{\"with\":[\"part"));
    CHECK(socket->handlerCalls == 0);
    CHECK(feed(socket, "icipants\"]}"));
    CHECK(socket->handlerCalls == 1);
    CHECK(strcmp(socket->lastBody, "{\"with\":[\"participants\"]}") == 0);
    CHECK(output_has(socket, "Content-Length: 25\r\n"));
    CHECK(output_has(socket, "\r\n\r\n{\"with\":[\"participants\"]}"));
    CHECK(response_framing_consistent(socket));
    CHECK(socket->connection.inputLength == 0);
    free(socket);
}

static void test_chunked_body_in_place(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    const char request[] =
        "POST /echo?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
        "Host: fixture\r\nTransfer-Encoding: chunked\r\n\r\n"
        "6;ext=1\r\n{\"with\r\n"
        "8\r\n\":[1,2]}\r\n"
        "0\r\nTrailer: yes\r\n\r\n"
        "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n";
    CHECK(feed(socket, request));
    CHECK(socket->handlerCalls == 2);
    CHECK(output_has(socket, "\r\n\r\n{\"with\":[1,2]}HTTP/1.1 200 OK\r\n"));
    CHECK(output_has(socket, "\"data\":\"pong\"}"));
    CHECK(socket->connection.inputLength == 0);
    CHECK(socket->connection.requestsServed == 2);

    reset_output(socket);
    const char badChunk[] =
        "POST /echo?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n\r\nZZ\r\nx\r\n0\r\n\r\n";
    CHECK(!feed(socket, badChunk));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 400 Bad Request\r\n", 26) == 0);
    CHECK(bb_connection_wants_close(&socket->connection));
    free(socket);
}

static void test_pipelined_requests(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    const char request[] =
        "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\nHost: a\r\n\r\n"
        "GET /api/v1/server/info?guid=" TEST_PASSWORD " HTTP/1.1\r\nHost: b\r\n\r\n"
        "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\nHost: c\r\n\r\n";
    CHECK(feed(socket, request));
    CHECK(socket->handlerCalls == 3);
    CHECK(output_count(socket, "HTTP/1.1 200 OK\r\n") == 3);
    CHECK(output_count(socket, "\"data\":\"pong\"") == 2);
    CHECK(output_count(socket, "\"server_version\":\"0.2.0\"") == 1);
    const char *first = strstr(output_text(socket), "\"data\":\"pong\"");
    const char *second = strstr(output_text(socket), "\"server_version\"");
    const char *third = strstr(second, "\"data\":\"pong\"");
    CHECK(first && second && third && first < second && second < third);
    CHECK(socket->connection.inputLength == 0);
    CHECK(socket->connection.requestsServed == 3);

    /* A pipelined request that is split mid-way must be retained intact. */
    reset_output(socket);
    const char split[] =
        "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"
        "GET /api/v1/server/inf";
    CHECK(feed(socket, split));
    CHECK(socket->connection.requestsServed == 4);
    CHECK(socket->connection.inputLength == strlen("GET /api/v1/server/inf"));
    CHECK(memcmp(socket->input, "GET /api/v1/server/inf",
                 socket->connection.inputLength) == 0);
    CHECK(feed(socket, "o?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(socket->connection.requestsServed == 5);
    CHECK(output_has(socket, "\"os_version\":\"9.3.5\""));
    free(socket);
}

static void test_malformed_framing(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    CHECK(!feed(socket, "NOT HTTP AT ALL\r\n\r\n"));
    CHECK(socket->handlerCalls == 0);
    CHECK(strncmp(output_text(socket), "HTTP/1.1 400 Bad Request\r\n", 26) == 0);
    CHECK(output_has(socket, "Connection: close\r\n"));
    CHECK(output_has(socket,
        "{\"status\":400,\"message\":\"Bad Request\",\"error\":{\"type\":"
        "\"Validation Error\",\"message\":\"Malformed HTTP request\"}}"));
    CHECK(response_framing_consistent(socket));
    CHECK(bb_connection_wants_close(&socket->connection));
    CHECK(socket->connection.requestsRejected == 1);
    /* Nothing after a rejection is parsed. */
    reset_output(socket);
    CHECK(!feed(socket, "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(socket->outputLength == 0 && socket->handlerCalls == 0);

    socket_init(socket);
    CHECK(!feed(socket, "POST /echo?guid=x HTTP/1.1\r\nContent-Length: 1\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n"));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 400 ", 13) == 0);

    socket_init(socket);
    CHECK(!feed(socket, "GET /echo HTTP/1.1\r\nBad Header\r\n\r\n"));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 400 ", 13) == 0);
    free(socket);
}

static void test_authentication(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    CHECK(feed(socket, "GET /api/v1/ping HTTP/1.1\r\n\r\n"));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 401 Unauthorized\r\n", 27) == 0);
    CHECK(output_has(socket,
        "{\"status\":401,\"message\":\"You are not authorized to access this resource\","
        "\"error\":{\"type\":\"Authentication Error\",\"message\":\"Missing server password!\"}}"));
    CHECK(response_framing_consistent(socket));
    CHECK(!bb_connection_wants_close(&socket->connection));

    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?guid=wrong-value HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "\"message\":\"Unauthorized\"}}"));

    /* Same length, different bytes. */
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?guid=fixture-onlx HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 401 "));
    CHECK(output_has(socket, "\"message\":\"Unauthorized\"}}"));

    /* Prefix of the password. */
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?guid=fixture HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 401 "));

    /* Empty guid counts as missing, like the reference middleware. */
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?guid= HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "Missing server password!"));

    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?password=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 200 OK\r\n"));

    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?other=1&token=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 200 OK\r\n"));

    /* Percent-encoded password. */
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?guid=fixture%2Donly HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 200 OK\r\n"));

    /* Malformed escape is a failed authentication, not a crash. */
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?guid=fixture%2 HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 401 "));

    /* Unknown routes still authenticate first. */
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/chat/count HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 401 "));

    /* Unconfigured password never authenticates. */
    socket->config.password = "";
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping?guid= HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 500 "));
    CHECK(output_has(socket, "\"type\":\"Server Error\""));
    CHECK(!bb_router_authenticate("guid=", ""));
    CHECK(!bb_router_authenticate("guid=", NULL));
    CHECK(bb_router_authenticate("guid=" TEST_PASSWORD, TEST_PASSWORD));
    CHECK(!bb_router_authenticate(NULL, TEST_PASSWORD));
    free(socket);
}

static void test_options_and_routes(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    CHECK(feed(socket, "OPTIONS /api/v1/chat/query HTTP/1.1\r\n"
                       "Origin: http://fixture\r\n\r\n"));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 204 No Content\r\n", 25) == 0);
    CHECK(output_has(socket, "Content-Length: 0\r\n"));
    CHECK(!output_has(socket, "Content-Type:"));
    CHECK(output_has(socket, "Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\n"));
    CHECK(output_has(socket, "Access-Control-Allow-Headers: Content-Type,Range\r\n"));
    CHECK(response_framing_consistent(socket));
    CHECK(socket->handlerCalls == 1);

    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/server/info?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 200 OK\r\n"));
    CHECK(output_has(socket, "\r\n\r\n{\"status\":200,\"message\":\"Success\",\"data\":{"));
    CHECK(output_has(socket, "\"computer_id\":\"bluebubbles-ios-host-test\""));
    CHECK(output_has(socket, "\"os_version\":\"9.3.5\""));
    CHECK(output_has(socket, "\"server_version\":\"0.2.0\""));
    CHECK(output_has(socket, "\"private_api\":false"));
    CHECK(output_has(socket, "\"helper_connected\":false"));
    CHECK(output_has(socket, "\"proxy_service\":\"Direct\""));
    CHECK(output_has(socket, "\"detected_icloud\":\"\""));
    CHECK(output_has(socket, "\"detected_imessage\":\"\""));
    CHECK(output_has(socket, "\"macos_time_sync\":null"));
    CHECK(output_has(socket, "\"local_ipv4s\":[\"127.0.0.1\"]"));
    CHECK(output_has(socket, "\"local_ipv6s\":[]"));
    CHECK(output_has(socket, "\"ios_capabilities\":{\"text\":true"));
    CHECK(output_has(socket, "\"reactions\":false"));
    CHECK(output_has(socket, "\"scheduling\":false}}}"));
    CHECK(response_framing_consistent(socket));

    /* Trailing slash tolerance. */
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/ping/?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "\"data\":\"pong\""));

    reset_output(socket);
    CHECK(feed(socket, "POST /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                       "Content-Length: 0\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 405 Method Not Allowed\r\n"));
    CHECK(output_has(socket, "\"status\":405"));

    /* Bridge-backed routes without a request table answer 503, never pending. */
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/chat/count?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 503 Service Unavailable\r\n"));
    CHECK(output_has(socket, "\"message\":\"The messaging bridge is unavailable\"}}"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);

    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/nothing/here?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 404 Not Found\r\n"));
    CHECK(output_has(socket,
        "{\"status\":404,\"message\":\"Not Found\",\"error\":{\"type\":"
        "\"Validation Error\",\"message\":\"Route not found\"}}"));

    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/fcm/client?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 404 Not Found\r\n"));
    CHECK(output_has(socket, "FCM is not supported by the iOS 9 bridge"));

    reset_output(socket);
    CHECK(feed(socket, "GET /?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 404 Not Found\r\n"));
    CHECK(!bb_connection_wants_close(&socket->connection));

    /* Metadata defaults when nothing is configured. */
    memset(&socket->config.metadata, 0, sizeof(socket->config.metadata));
    reset_output(socket);
    CHECK(feed(socket, "GET /api/v1/server/info?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "\"computer_id\":\"bluebubbles-ios-bridge\""));
    CHECK(output_has(socket, "\"server_version\":\"0.2.0\""));
    CHECK(output_has(socket, "\"local_ipv4s\":[]"));
    free(socket);
}

static void test_keep_alive_and_close(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    /* HTTP/1.0 without keep-alive closes after one response. */
    CHECK(!feed(socket, "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.0\r\n\r\n"
                        "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.0\r\n\r\n"));
    CHECK(socket->handlerCalls == 1);
    CHECK(output_has(socket, "Connection: close\r\n"));
    CHECK(bb_connection_wants_close(&socket->connection));
    CHECK(socket->connection.inputLength == 0);

    socket_init(socket);
    CHECK(feed(socket, "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.0\r\n"
                       "Connection: keep-alive\r\n\r\n"));
    CHECK(output_has(socket, "Connection: keep-alive\r\n"));
    CHECK(!bb_connection_wants_close(&socket->connection));

    socket_init(socket);
    CHECK(!feed(socket, "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                        "Connection: close\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 200 OK\r\n"));
    CHECK(output_has(socket, "Connection: close\r\n"));
    CHECK(bb_connection_wants_close(&socket->connection));
    CHECK(!feed(socket, ""));

    /* A failed emit closes the connection. */
    socket_init(socket);
    socket->failEmit = true;
    CHECK(!feed(socket, "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(bb_connection_wants_close(&socket->connection));
    CHECK(socket->emitCalls == 1);

    /* A handler failure answers 500 and closes. */
    socket_init(socket);
    CHECK(!feed(socket, "GET /fail HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 500 Internal Server Error\r\n"));
    CHECK(output_has(socket, "\"status\":500"));
    CHECK(bb_connection_wants_close(&socket->connection));
    free(socket);
}

static void test_oversized_input(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    static unsigned char smallInput[300];
    static char scratch[512];
    BBConnectionCallbacks callbacks;
    socket_init(socket);
    callbacks = socket->connection.callbacks;
    CHECK(bb_connection_init(&socket->connection, smallInput, sizeof(smallInput),
                             scratch, sizeof(scratch), &callbacks));

    /* Headers that never complete within the buffer. */
    char filler[400];
    memset(filler, 'a', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\0';
    char request[512];
    snprintf(request, sizeof(request), "GET /api/v1/ping?guid=%s HTTP/1.1\r\nX-Filler: ", TEST_PASSWORD);
    CHECK(feed(socket, request));
    CHECK(!feed(socket, filler));
    CHECK(output_has(socket, "HTTP/1.1 413 Payload Too Large\r\n"));
    CHECK(output_has(socket, "\"status\":413"));
    CHECK(bb_connection_wants_close(&socket->connection));
    CHECK(socket->handlerCalls == 0);

    /* A body larger than the buffer, delivered gradually. */
    CHECK(bb_connection_init(&socket->connection, smallInput, sizeof(smallInput),
                             scratch, sizeof(scratch), &callbacks));
    reset_output(socket);
    CHECK(feed(socket, "POST /echo?guid=x HTTP/1.1\r\nContent-Length: 1000\r\n\r\n"));
    bool alive = true;
    for (int i = 0; i < 400 && alive; i++) alive = feed(socket, "b");
    CHECK(!alive);
    CHECK(output_has(socket, "HTTP/1.1 413 "));
    CHECK(socket->handlerCalls == 0);

    /* Too small a scratch buffer is refused at init. */
    CHECK(!bb_connection_init(&socket->connection, smallInput, sizeof(smallInput),
                              scratch, 100, &callbacks));
    CHECK(bb_connection_wants_close(&socket->connection));
    free(socket);
}

static void test_independent_connections(void)
{
    TestSocket *a = malloc(sizeof(*a));
    TestSocket *b = malloc(sizeof(*b));
    socket_init(a);
    socket_init(b);
    const char requestA[] =
        "POST /echo?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 9\r\n\r\nalpha-one";
    const char requestB[] =
        "GET /api/v1/server/info?guid=" TEST_PASSWORD " HTTP/1.1\r\nHost: b\r\n\r\n";
    size_t cursorA = 0;
    size_t cursorB = 0;
    size_t lengthA = strlen(requestA);
    size_t lengthB = strlen(requestB);
    /* Interleave uneven fragments so neither connection sees a whole
     * request until its own final piece arrives. */
    while (cursorA < lengthA || cursorB < lengthB) {
        if (cursorA < lengthA) {
            size_t step = lengthA - cursorA < 7 ? lengthA - cursorA : 7;
            CHECK(bb_connection_receive(&a->connection,
                                        (const unsigned char *)requestA + cursorA, step));
            cursorA += step;
        }
        if (cursorB < lengthB) {
            size_t step = lengthB - cursorB < 5 ? lengthB - cursorB : 5;
            CHECK(bb_connection_receive(&b->connection,
                                        (const unsigned char *)requestB + cursorB, step));
            cursorB += step;
        }
    }
    CHECK(a->handlerCalls == 1 && b->handlerCalls == 1);
    CHECK(strcmp(a->lastPath, "/echo") == 0);
    CHECK(strcmp(b->lastPath, "/api/v1/server/info") == 0);
    CHECK(output_has(a, "\r\n\r\nalpha-one"));
    CHECK(!output_has(a, "server_version"));
    CHECK(output_has(b, "\"server_version\":\"0.2.0\""));
    CHECK(!output_has(b, "alpha-one"));
    CHECK(a->connection.inputLength == 0 && b->connection.inputLength == 0);
    CHECK(!bb_connection_wants_close(&a->connection));
    CHECK(!bb_connection_wants_close(&b->connection));

    /* Closing one connection leaves the other serviceable. */
    CHECK(!feed(a, "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\nConnection: close\r\n\r\n"));
    reset_output(b);
    CHECK(feed(b, "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(b, "\"data\":\"pong\""));
    free(a);
    free(b);
}

static void test_upgrade_handoff(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    socket_init(socket);
    const char request[] =
        "GET /socket.io/?EIO=4&transport=websocket&guid=" TEST_PASSWORD " HTTP/1.1\r\n"
        "Host: fixture\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: fixture-key\r\n\r\n"
        "\x81\x02" "40";
    CHECK(bb_connection_receive(&socket->connection,
                                (const unsigned char *)request, sizeof(request) - 1));
    CHECK(strncmp(output_text(socket), "HTTP/1.1 101 Switching Protocols\r\n", 34) == 0);
    CHECK(output_has(socket, "Connection: Upgrade\r\n"));
    CHECK(output_has(socket, "Upgrade: websocket\r\n"));
    CHECK(output_has(socket, "Sec-WebSocket-Accept: fixture-accept\r\n\r\n"));
    CHECK(!output_has(socket, "Content-Length"));
    CHECK(!output_has(socket, "Access-Control"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateUpgraded);
    /* Frame bytes that arrived with the handshake bypass the HTTP parser. */
    CHECK(socket->upgradedLength == 4);
    CHECK(memcmp(socket->upgraded, "\x81\x02" "40", 4) == 0);
    CHECK(socket->connection.inputLength == 0);

    CHECK(feed(socket, "\x81\x01" "3"));
    CHECK(socket->upgradedLength == 7);
    CHECK(socket->connection.upgradedBytes == 7);
    reset_output(socket);
    CHECK(bb_connection_send_upgraded(&socket->connection,
                                      (const unsigned char *)"\x81\x01" "2", 3));
    CHECK(socket->outputLength == 3);

    /* Without Upgrade headers the router answers normally and stays HTTP. */
    socket_init(socket);
    CHECK(feed(socket, "GET /socket.io/?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 404 Not Found\r\n"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
    CHECK(bb_connection_send_upgraded(&socket->connection,
                                      (const unsigned char *)"x", 1) == false);

    /* A handler answering 101 to a non-upgrade request is a handler error. */
    socket_init(socket);
    CHECK(!feed(socket, "GET /force-upgrade HTTP/1.1\r\n\r\n"));
    CHECK(output_has(socket, "HTTP/1.1 500 Internal Server Error\r\n"));
    CHECK(bb_connection_wants_close(&socket->connection));
    CHECK(socket->upgradedLength == 0);
    free(socket);
}

static void test_pending_response(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    BBHTTPResponse response;
    size_t bodyLength = 0;
    const unsigned char *body;
    socket_init(socket);
    pendingCallbacks = 0;

    /* Handler defers; the request and body stay readable. */
    CHECK(feed(socket, "POST /pending?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                       "Content-Length: 5\r\n\r\nhello"
                       "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(pendingCallbacks == 1 && lastPendingContext == socket);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStatePending);
    CHECK(socket->outputLength == 0 && socket->handlerCalls == 1);
    CHECK(socket->connection.pendingResponses == 1);
    body = bb_connection_pending_body(&socket->connection, &bodyLength);
    CHECK(body && bodyLength == 5 && memcmp(body, "hello", 5) == 0);
    CHECK(strcmp(socket->connection.request.path, "/pending") == 0);

    /* More input while pending is buffered only. */
    CHECK(feed(socket, "GET /api/v1/server/info?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(socket->handlerCalls == 1 && socket->outputLength == 0);

    /* Completion emits, then the two buffered requests are served. */
    bb_response_init(&response);
    response.contentType = BB_RESPONSE_CONTENT_TYPE_TEXT;
    response.body = (const unsigned char *)"later";
    response.bodyLength = 5;
    CHECK(bb_connection_complete(&socket->connection, &response));
    CHECK(socket->handlerCalls == 3);
    CHECK(strncmp(output_text(socket), "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(output_has(socket, "\r\n\r\nlaterHTTP/1.1 200 OK"));
    CHECK(output_has(socket, "\"data\":\"pong\""));
    CHECK(output_has(socket, "\"server_version\":\"0.2.0\""));
    CHECK(socket->connection.requestsServed == 3);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
    CHECK(bb_connection_pending_body(&socket->connection, &bodyLength) == NULL && bodyLength == 0);

    /* Completing when nothing is pending is refused. */
    CHECK(!bb_connection_complete(&socket->connection, &response));

    /* Completing with close semantics closes after emitting. */
    reset_output(socket);
    CHECK(feed(socket, "GET /pending?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    response.closeConnection = true;
    CHECK(!bb_connection_complete(&socket->connection, &response));
    CHECK(output_has(socket, "Connection: close\r\n"));
    CHECK(bb_connection_wants_close(&socket->connection));

    /* Completing with another pending response is a handler error. */
    socket_init(socket);
    CHECK(feed(socket, "GET /pending?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    bb_response_init(&response);
    response.pending = true;
    CHECK(!bb_connection_complete(&socket->connection, &response));
    CHECK(output_has(socket, "HTTP/1.1 500 "));

    /* Buffer exhaustion while pending still answers 413 and closes. */
    socket_init(socket);
    CHECK(feed(socket, "GET /pending?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    {
        static char filler[INPUT_CAPACITY + 1];
        memset(filler, 'x', sizeof(filler) - 1);
        filler[sizeof(filler) - 1] = '\0';
        CHECK(!feed(socket, filler));
    }
    CHECK(output_has(socket, "HTTP/1.1 413 "));
    free(socket);
}

/* ---- body streaming ------------------------------------------------- */

typedef struct {
    unsigned begins;
    unsigned ends;
    unsigned aborts;
    unsigned refuseAfterBytes;   /* 0 = never refuse */
    bool endPending;             /* streamEnd leaves the response pending */
    bool endFails;
    size_t received;
    unsigned char digest;        /* xor of every streamed byte */
    unsigned long long sum;      /* sum of every streamed byte */
    char path[128];
} StreamProbe;

static StreamProbe streamProbe;

static bool stream_begin(void *context, BBConnection *connection,
                         const BBHTTPRequest *request, void **streamContext)
{
    (void)context;
    (void)connection;
    if (strcmp(request->path, "/upload") != 0) return false;
    streamProbe.begins++;
    streamProbe.received = 0;
    streamProbe.digest = 0;
    streamProbe.sum = 0;
    strcpy(streamProbe.path, request->path);
    *streamContext = &streamProbe;
    return true;
}

static bool stream_data(void *context, BBConnection *connection, void *streamContext,
                        const unsigned char *bytes, size_t length)
{
    StreamProbe *probe = (StreamProbe *)streamContext;
    (void)context;
    (void)connection;
    CHECK(probe == &streamProbe);
    for (size_t i = 0; i < length; i++) {
        probe->digest ^= bytes[i];
        probe->sum += bytes[i];
    }
    probe->received += length;
    return !probe->refuseAfterBytes || probe->received < probe->refuseAfterBytes;
}

static bool stream_end(void *context, BBConnection *connection, void *streamContext,
                       char *scratch, size_t scratchCapacity, BBHTTPResponse *response)
{
    StreamProbe *probe = (StreamProbe *)streamContext;
    int written;
    (void)context;
    (void)connection;
    CHECK(probe == &streamProbe);
    probe->ends++;
    if (probe->endFails) return false;
    if (probe->endPending) {
        response->pending = true;
        response->handlerContext = probe;
        return true;
    }
    written = snprintf(scratch, scratchCapacity, "streamed %zu bytes digest %u sum %llu",
                       probe->received, (unsigned)probe->digest, probe->sum);
    response->contentType = BB_RESPONSE_CONTENT_TYPE_TEXT;
    response->body = (const unsigned char *)scratch;
    response->bodyLength = (size_t)written;
    return true;
}

static void stream_abort(void *context, BBConnection *connection, void *streamContext)
{
    (void)context;
    (void)connection;
    CHECK(streamContext == &streamProbe);
    streamProbe.aborts++;
}

static void streaming_socket_init(TestSocket *socket)
{
    BBConnectionCallbacks callbacks;
    socket_init(socket);
    callbacks = socket->connection.callbacks;
    callbacks.streamBegin = stream_begin;
    callbacks.streamData = stream_data;
    callbacks.streamEnd = stream_end;
    callbacks.streamAbort = stream_abort;
    socket->connection.callbacks = callbacks;
    memset(&streamProbe, 0, sizeof(streamProbe));
}

static void test_body_streaming(void)
{
    TestSocket *socket = malloc(sizeof(*socket));
    static unsigned char body[3 * INPUT_CAPACITY];
    unsigned char digest = 0;
    unsigned long long sum = 0;
    char head[256];
    size_t headLength;
    BBHTTPResponse response;

    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = (unsigned char)(i * 7 + 3);
        digest ^= body[i];
        sum += body[i];
    }
    headLength = (size_t)snprintf(head, sizeof(head),
        "POST /upload?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: %zu\r\n\r\n",
        sizeof(body));

    /* A body three times the input buffer, delivered in odd-sized pieces
     * with a pipelined request glued to the end. */
    streaming_socket_init(socket);
    CHECK(bb_connection_receive(&socket->connection, (const unsigned char *)head, headLength - 3));
    CHECK(streamProbe.begins == 0);
    CHECK(bb_connection_receive(&socket->connection, (const unsigned char *)head + headLength - 3, 3));
    CHECK(streamProbe.begins == 1 && strcmp(streamProbe.path, "/upload") == 0);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateStreaming);
    CHECK(socket->connection.inputLength == 0);
    {
        size_t offset = 0;
        size_t piece = 1;
        while (offset < sizeof(body) - 100) {
            size_t take = piece;
            if (take > sizeof(body) - 100 - offset) take = sizeof(body) - 100 - offset;
            CHECK(bb_connection_receive(&socket->connection, body + offset, take));
            offset += take;
            piece = piece * 3 + 1;
            if (piece > 5000) piece = 1;
        }
        CHECK(bb_connection_state(&socket->connection) == BBConnectionStateStreaming);
        CHECK(socket->outputLength == 0);
        /* Last 100 body bytes plus a pipelined ping in one read. */
        {
            unsigned char tail[100 + 128];
            const char ping[] = "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n";
            memcpy(tail, body + offset, 100);
            memcpy(tail + 100, ping, strlen(ping));
            CHECK(bb_connection_receive(&socket->connection, tail, 100 + strlen(ping)));
        }
    }
    CHECK(streamProbe.ends == 1 && streamProbe.aborts == 0);
    CHECK(streamProbe.received == sizeof(body));
    CHECK(streamProbe.digest == digest && streamProbe.sum == sum);
    CHECK(strncmp(output_text(socket), "HTTP/1.1 200 OK\r\n", 17) == 0);
    {
        char expected[128];
        snprintf(expected, sizeof(expected), "streamed %zu bytes digest %u sum %llu",
                 sizeof(body), (unsigned)digest, sum);
        CHECK(output_has(socket, expected));
    }
    CHECK(output_has(socket, "\"data\":\"pong\""));
    CHECK(socket->connection.requestsServed == 2);
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateHTTP);
    CHECK(socket->handlerCalls == 1);   /* only the ping went through handleRequest */

    /* A small body that arrived whole is streamed too (one code path). */
    reset_output(socket);
    streamProbe.begins = streamProbe.ends = 0;
    CHECK(feed(socket, "POST /upload?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello"
                       "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(streamProbe.begins == 1 && streamProbe.ends == 1 && streamProbe.received == 5);
    CHECK(output_has(socket, "streamed 5 bytes"));
    CHECK(output_has(socket, "\"data\":\"pong\""));
    CHECK(socket->connection.requestsServed == 4);

    /* Other routes are untouched by the stream callbacks. */
    reset_output(socket);
    streamProbe.begins = 0;
    CHECK(feed(socket, "POST /echo?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc"));
    CHECK(streamProbe.begins == 0 && socket->handlerCalls == 3);

    /* streamEnd may leave the response pending; the completion follows the
     * usual path and pipelined input waits. */
    reset_output(socket);
    pendingCallbacks = 0;
    streamProbe.endPending = true;
    CHECK(feed(socket, "POST /upload?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 4\r\n\r\nda"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStateStreaming);
    CHECK(feed(socket, "ta" "GET /api/v1/ping?guid=" TEST_PASSWORD " HTTP/1.1\r\n\r\n"));
    CHECK(bb_connection_state(&socket->connection) == BBConnectionStatePending);
    CHECK(pendingCallbacks == 1 && lastPendingContext == &streamProbe);
    CHECK(socket->outputLength == 0);
    bb_response_init(&response);
    response.contentType = BB_RESPONSE_CONTENT_TYPE_TEXT;
    response.body = (const unsigned char *)"done";
    response.bodyLength = 4;
    CHECK(bb_connection_complete(&socket->connection, &response));
    CHECK(output_has(socket, "\r\n\r\ndoneHTTP/1.1 200 OK"));
    CHECK(output_has(socket, "\"data\":\"pong\""));
    streamProbe.endPending = false;

    /* Refusing data answers 400, aborts, and closes. */
    streaming_socket_init(socket);
    streamProbe.refuseAfterBytes = 10;
    CHECK(bb_connection_receive(&socket->connection, (const unsigned char *)head, headLength));
    CHECK(!bb_connection_receive(&socket->connection, body, 64));
    CHECK(output_has(socket, "HTTP/1.1 400 "));
    CHECK(output_has(socket, "Upload refused"));
    CHECK(streamProbe.aborts == 1 && streamProbe.ends == 0);
    CHECK(bb_connection_wants_close(&socket->connection));

    /* The socket closing mid-stream aborts. */
    streaming_socket_init(socket);
    CHECK(bb_connection_receive(&socket->connection, (const unsigned char *)head, headLength));
    CHECK(bb_connection_receive(&socket->connection, body, 1000));
    bb_connection_abort(&socket->connection);
    CHECK(streamProbe.aborts == 1 && bb_connection_wants_close(&socket->connection));
    CHECK(!bb_connection_receive(&socket->connection, body, 1));

    /* A failing streamEnd is a handler error: 500, abort, close. */
    streaming_socket_init(socket);
    streamProbe.endFails = true;
    CHECK(!feed(socket, "POST /upload?guid=" TEST_PASSWORD " HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi"));
    CHECK(output_has(socket, "HTTP/1.1 500 ") && streamProbe.aborts == 1);

    /* Chunked bodies are never offered for streaming. */
    streaming_socket_init(socket);
    CHECK(feed(socket, "POST /upload?guid=" TEST_PASSWORD " HTTP/1.1\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n"));
    CHECK(streamProbe.begins == 0 && socket->handlerCalls == 1);
    free(socket);
}

static void test_response_builders(void)
{
    char buffer[512];
    size_t length = 0;
    BBJSONWriter writer;

    CHECK(bb_response_error_envelope(buffer, sizeof(buffer), &length, 400,
                                     "Bad Request", "Validation Error",
                                     "quote\" back\\ nl\n tab\t ctl\x01 del\x7f"));
    CHECK(strcmp(buffer,
        "{\"status\":400,\"message\":\"Bad Request\",\"error\":{\"type\":\"Validation Error\","
        "\"message\":\"quote\\\" back\\\\ nl\\n tab\\t ctl\\u0001 del\\u007f\"}}") == 0);
    CHECK(length == strlen(buffer));
    CHECK(bb_response_success_envelope(buffer, sizeof(buffer), &length, NULL, "[1,2]"));
    CHECK(strcmp(buffer, "{\"status\":200,\"message\":\"Success\",\"data\":[1,2]}") == 0);
    CHECK(!bb_response_success_envelope(buffer, 16, &length, NULL, "[1,2]"));
    CHECK(!bb_response_error_envelope(buffer, sizeof(buffer), &length, 99, NULL, NULL, NULL));

    bb_json_writer_init(&writer, buffer, sizeof(buffer));
    CHECK(bb_json_write_raw(&writer, "[") &&
          bb_json_write_unsigned(&writer, 0) && bb_json_write_raw(&writer, ",") &&
          bb_json_write_unsigned(&writer, 9) && bb_json_write_raw(&writer, ",") &&
          bb_json_write_unsigned(&writer, 10) && bb_json_write_raw(&writer, ",") &&
          bb_json_write_unsigned(&writer, UINT64_C(1758067200123)) &&
          bb_json_write_raw(&writer, ",") &&
          bb_json_write_unsigned(&writer, UINT64_MAX) && bb_json_write_raw(&writer, ",") &&
          bb_json_write_signed(&writer, -1) && bb_json_write_raw(&writer, ",") &&
          bb_json_write_signed(&writer, INT64_MIN) && bb_json_write_raw(&writer, ",") &&
          bb_json_write_key(&writer, "k\"ey") && bb_json_write_bool(&writer, true) &&
          bb_json_write_raw(&writer, ",") && bb_json_write_string(&writer, NULL) &&
          bb_json_write_raw(&writer, ",") && bb_json_write_string(&writer, "caf\xc3\xa9") &&
          bb_json_write_raw(&writer, "]") && bb_json_writer_finish(&writer, &length));
    CHECK(strcmp(buffer,
        "[0,9,10,1758067200123,18446744073709551615,-1,-9223372036854775808,"
        "\"k\\\"ey\":true,null,\"caf\xc3\xa9\"]") == 0);

    /* Overflow is sticky and finish reports it. */
    bb_json_writer_init(&writer, buffer, 8);
    CHECK(!bb_json_write_raw(&writer, "0123456789"));
    CHECK(!bb_json_write_raw(&writer, ""));
    CHECK(!bb_json_writer_finish(&writer, &length));

    BBHTTPResponse response;
    bb_response_init(&response);
    response.status = 206;
    response.contentType = "image/jpeg";
    response.body = (const unsigned char *)"abc";
    response.bodyLength = 3;
    response.extraHeaders = "Accept-Ranges: bytes\r\nContent-Range: bytes 0-2/10\r\n";
    CHECK(bb_response_write_head(&response, true, buffer, sizeof(buffer), &length));
    CHECK(strcmp(buffer,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: 3\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type,Range\r\n"
        "Accept-Ranges: bytes\r\nContent-Range: bytes 0-2/10\r\n\r\n") == 0);
    CHECK(length == strlen(buffer));
    response.extraHeaders = "Injected\r\n";
    CHECK(!bb_response_write_head(&response, true, buffer, sizeof(buffer), &length));
    response.extraHeaders = "X: a\nb\r\n";
    CHECK(!bb_response_write_head(&response, true, buffer, sizeof(buffer), &length));
    response.extraHeaders = NULL;
    CHECK(!bb_response_write_head(&response, true, buffer, 64, &length));
    response.body = NULL;
    CHECK(!bb_response_write_head(&response, true, buffer, sizeof(buffer), &length));
    CHECK(strcmp(bb_response_status_text(999), "Unknown") == 0);
}

int main(void)
{
    test_ping_fragmented_headers();
    test_fragmented_body_and_echo();
    test_chunked_body_in_place();
    test_pipelined_requests();
    test_malformed_framing();
    test_authentication();
    test_options_and_routes();
    test_keep_alive_and_close();
    test_oversized_input();
    test_independent_connections();
    test_upgrade_handoff();
    test_pending_response();
    test_body_streaming();
    test_response_builders();
    if (failures) return 1;
    puts("connection tests passed: fragmentation, pipelining, keep-alive, auth, routes, upgrade, independent connections");
    return 0;
}
