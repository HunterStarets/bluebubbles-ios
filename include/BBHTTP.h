#ifndef BB_HTTP_H
#define BB_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_HTTP_MAX_HEADER_BYTES (16U * 1024U)
#define BB_HTTP_MAX_BODY_BYTES (32U * 1024U * 1024U)
#define BB_HTTP_MAX_TARGET_BYTES 4096U
#define BB_HTTP_MAX_PATH_BYTES 3072U
#define BB_HTTP_MAX_QUERY_BYTES 2048U
#define BB_HTTP_MAX_CONTENT_TYPE_BYTES 256U
#define BB_HTTP_MAX_WEBSOCKET_KEY_BYTES 64U

typedef enum {
    BBHTTPParseInvalid = -1,
    BBHTTPParseNeedMore = 0,
    BBHTTPParseReady = 1
} BBHTTPParseResult;

typedef enum {
    BBHTTPMethodUnknown = 0,
    BBHTTPMethodGET,
    BBHTTPMethodPOST,
    BBHTTPMethodPUT,
    BBHTTPMethodDELETE,
    BBHTTPMethodOPTIONS
} BBHTTPMethod;

typedef struct {
    BBHTTPMethod method;
    bool http11;
    bool keepAlive;
    bool chunked;
    bool upgradeWebSocket;
    /* Sec-WebSocket-Version: 13 was present. */
    bool webSocketVersion13;
    /* The head parsed completely. On BBHTTPParseNeedMore with this set,
     * a Content-Length body is still arriving: bodyLength/messageLength
     * already hold the expected totals so the caller may stream it. */
    bool headComplete;
    size_t headerLength;
    size_t bodyOffset;
    size_t bodyLength;
    size_t decodedBodyLength;
    size_t messageLength;
    char target[BB_HTTP_MAX_TARGET_BYTES];
    char path[BB_HTTP_MAX_PATH_BYTES];
    char query[BB_HTTP_MAX_QUERY_BYTES];
    char contentType[BB_HTTP_MAX_CONTENT_TYPE_BYTES];
    /* Sec-WebSocket-Key value, empty when absent. */
    char webSocketKey[BB_HTTP_MAX_WEBSOCKET_KEY_BYTES];
} BBHTTPRequest;

/* Parse one complete or partial HTTP/1.0 or HTTP/1.1 request. The returned
 * pointers are all represented as bounded copies in BBHTTPRequest, so the
 * caller may compact its input buffer after BBHTTPParseReady. */
BBHTTPParseResult bb_http_parse_request(const unsigned char *buffer,
                                        size_t bufferLength,
                                        BBHTTPRequest *out);

/* Decode the body of a request that used Transfer-Encoding: chunked. The
 * encoded body begins at request.bodyOffset and spans request.bodyLength. */
bool bb_http_decode_chunked_body(const unsigned char *encoded,
                                 size_t encodedLength,
                                 unsigned char *out,
                                 size_t outCapacity,
                                 size_t *outLength);

/* Decode a chunked body in place. Decoded bytes never overtake the encoded
 * read position, so no second body buffer is required. */
bool bb_http_decode_chunked_body_in_place(unsigned char *encoded,
                                          size_t encodedLength,
                                          size_t *outLength);

/* Find and URL-decode the first matching query parameter. '+' maps to space;
 * malformed percent escapes and decoded NUL bytes are rejected. */
bool bb_http_query_value(const char *query,
                         const char *name,
                         char *out,
                         size_t outCapacity,
                         size_t *outLength);

/* Percent-decode a path segment or query component. '+' becomes a space
 * only when plusIsSpace is set (query semantics). Malformed escapes and
 * decoded NUL bytes are rejected. */
bool bb_http_percent_decode(const char *source,
                            size_t length,
                            bool plusIsSpace,
                            char *out,
                            size_t outCapacity,
                            size_t *outLength);

const char *bb_http_method_name(BBHTTPMethod method);

#ifdef __cplusplus
}
#endif

#endif
