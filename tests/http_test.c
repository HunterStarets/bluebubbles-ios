#include "BBHTTP.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static BBHTTPParseResult parse_text(const char *text, BBHTTPRequest *request)
{
    return bb_http_parse_request((const unsigned char *)text, strlen(text), request);
}

static void test_content_length_request(void)
{
    const char requestText[] =
        "POST /api/v1/chat/query?guid=fixture%2Donly HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 12\r\n"
        "Connection: keep-alive\r\n\r\n"
        "{\"limit\":25}";
    BBHTTPRequest request;
    CHECK(parse_text(requestText, &request) == BBHTTPParseReady);
    CHECK(request.method == BBHTTPMethodPOST);
    CHECK(strcmp(bb_http_method_name(request.method), "POST") == 0);
    CHECK(request.http11 && request.keepAlive && !request.chunked);
    CHECK(strcmp(request.path, "/api/v1/chat/query") == 0);
    CHECK(strcmp(request.query, "guid=fixture%2Donly") == 0);
    CHECK(strcmp(request.contentType, "application/json") == 0);
    CHECK(request.bodyLength == 12 && request.decodedBodyLength == 12);
    CHECK(request.messageLength == strlen(requestText));

    char value[64];
    size_t valueLength = 0;
    CHECK(bb_http_query_value(request.query, "guid", value, sizeof(value),
                              &valueLength));
    CHECK(valueLength == 12 && strcmp(value, "fixture-only") == 0);
}

static void test_partial_and_pipeline(void)
{
    const char requestText[] =
        "GET /api/v1/server/info?guid=x HTTP/1.1\r\nHost: fixture\r\n\r\n"
        "GET /api/v1/chat/count?guid=x HTTP/1.1\r\nHost: fixture\r\n\r\n";
    const char *boundary = strstr(requestText, "GET /api/v1/chat/count");
    BBHTTPRequest request;
    CHECK(bb_http_parse_request((const unsigned char *)requestText, 20,
                                &request) == BBHTTPParseNeedMore);
    CHECK(!request.headComplete);
    CHECK(parse_text(requestText, &request) == BBHTTPParseReady);
    CHECK(request.headComplete);
    CHECK(request.messageLength == (size_t)(boundary - requestText));
    CHECK(strcmp(request.path, "/api/v1/server/info") == 0);
    BBHTTPRequest second;
    CHECK(bb_http_parse_request((const unsigned char *)requestText + request.messageLength,
                                strlen(requestText) - request.messageLength,
                                &second) == BBHTTPParseReady);
    CHECK(strcmp(second.path, "/api/v1/chat/count") == 0);
}

static void test_chunked_request(void)
{
    const char requestText[] =
        "POST /api/v1/message/query?guid=x HTTP/1.1\r\n"
        "Host: fixture\r\nTransfer-Encoding: chunked\r\n\r\n"
        "6;fixture=yes\r\n{\"with\r\n"
        "8\r\n\":[1,2]}\r\n"
        "0\r\nFixture-Trailer: yes\r\n\r\n";
    BBHTTPRequest request;
    CHECK(parse_text(requestText, &request) == BBHTTPParseReady);
    CHECK(request.chunked && request.decodedBodyLength == 14);
    unsigned char decoded[32];
    size_t decodedLength = 0;
    CHECK(bb_http_decode_chunked_body(
        (const unsigned char *)requestText + request.bodyOffset,
        request.bodyLength, decoded, sizeof(decoded), &decodedLength));
    CHECK(decodedLength == 14);
    CHECK(memcmp(decoded, "{\"with\":[1,2]}", 14) == 0);

    CHECK(bb_http_parse_request((const unsigned char *)requestText,
                                strlen(requestText) - 2,
                                &request) == BBHTTPParseNeedMore);

    /* Bytes that cannot start a request line are refused at once rather
     * than buffered: a TLS ClientHello on the plain port, or noise. */
    {
        const unsigned char hello[] = { 0x16, 0x03, 0x01, 0x00, 0xf1, 0x01, 0x00, 0x00 };
        CHECK(bb_http_parse_request(hello, sizeof(hello), &request) == BBHTTPParseInvalid);
        CHECK(bb_http_parse_request((const unsigned char *)" GET / HTTP/1.1\r\n\r\n", 18,
                                    &request) == BBHTTPParseInvalid);
        CHECK(bb_http_parse_request((const unsigned char *)"get / HTTP/1.1\r\n\r\n", 17,
                                    &request) == BBHTTPParseInvalid);
        CHECK(bb_http_parse_request((const unsigned char *)"G", 1, &request) == BBHTTPParseNeedMore);
    }

    /* A Content-Length body still arriving: the head is complete and the
     * expected totals are known, so the body can be streamed. */
    {
        const char partial[] =
            "POST /api/v1/message/attachment?guid=x HTTP/1.1\r\n"
            "Content-Type: multipart/form-data; boundary=b\r\nContent-Length: 1000\r\n\r\nabc";
        CHECK(bb_http_parse_request((const unsigned char *)partial, strlen(partial),
                                    &request) == BBHTTPParseNeedMore);
        CHECK(request.headComplete);
        CHECK(request.method == BBHTTPMethodPOST);
        CHECK(request.bodyLength == 1000);
        CHECK(request.bodyOffset == request.headerLength);
        CHECK(request.messageLength == request.headerLength + 1000);
        CHECK(strcmp(request.contentType, "multipart/form-data; boundary=b") == 0);
    }
}

static void test_upgrade_and_http10(void)
{
    const char upgrade[] =
        "GET /socket.io/?EIO=4&transport=websocket&guid=x HTTP/1.1\r\n"
        "Host: fixture\r\nUpgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n\r\n";
    BBHTTPRequest request;
    CHECK(parse_text(upgrade, &request) == BBHTTPParseReady);
    CHECK(request.upgradeWebSocket && request.keepAlive);

    const char old[] = "GET /api/v1/ping HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    CHECK(parse_text(old, &request) == BBHTTPParseReady);
    CHECK(!request.http11 && request.keepAlive);

    const char close[] = "GET /api/v1/ping HTTP/1.1\r\nConnection: close\r\n\r\n";
    CHECK(parse_text(close, &request) == BBHTTPParseReady);
    CHECK(!request.keepAlive);
}

static void test_rejections(void)
{
    BBHTTPRequest request;
    const char conflict[] =
        "POST / HTTP/1.1\r\nContent-Length: 1\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    CHECK(parse_text(conflict, &request) == BBHTTPParseInvalid);

    const char duplicate[] =
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx";
    CHECK(parse_text(duplicate, &request) == BBHTTPParseInvalid);

    const char folded[] =
        "GET / HTTP/1.1\r\nHeader: value\r\n folded\r\n\r\n";
    CHECK(parse_text(folded, &request) == BBHTTPParseInvalid);

    const char badChunk[] =
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\nx\r\n0\r\n\r\n";
    CHECK(parse_text(badChunk, &request) == BBHTTPParseInvalid);

    const char unsupported[] = "PATCH / HTTP/1.1\r\n\r\n";
    CHECK(parse_text(unsupported, &request) == BBHTTPParseInvalid);

    const unsigned char controlHeader[] =
        "GET / HTTP/1.1\r\nX-Fixture: bad\0value\r\n\r\n";
    CHECK(bb_http_parse_request(controlHeader, sizeof(controlHeader) - 1,
                                &request) == BBHTTPParseInvalid);

    unsigned char oversizedHeader[BB_HTTP_MAX_HEADER_BYTES];
    for (size_t i = 0; i < sizeof(oversizedHeader); i++) oversizedHeader[i] = 'x';
    CHECK(bb_http_parse_request(oversizedHeader, sizeof(oversizedHeader),
                                &request) == BBHTTPParseInvalid);

    char value[16];
    CHECK(!bb_http_query_value("guid=bad%0", "guid", value, sizeof(value), NULL));
    CHECK(!bb_http_query_value("guid=%00", "guid", value, sizeof(value), NULL));
    CHECK(!bb_http_query_value("other=x", "guid", value, sizeof(value), NULL));
}

int main(void)
{
    test_content_length_request();
    test_partial_and_pipeline();
    test_chunked_request();
    test_upgrade_and_http10();
    test_rejections();
    if (failures) return 1;
    puts("http tests passed: incremental parse, keep-alive, chunked decode, query, upgrade");
    return 0;
}
