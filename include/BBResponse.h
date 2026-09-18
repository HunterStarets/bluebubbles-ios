#ifndef BB_RESPONSE_H
#define BB_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status line, framing headers, CORS, and any handler-supplied extra header
 * lines must fit in this many bytes. */
#define BB_RESPONSE_MAX_HEAD_BYTES 1024U

#define BB_RESPONSE_CONTENT_TYPE_JSON "application/json; charset=utf-8"
#define BB_RESPONSE_CONTENT_TYPE_TEXT "text/plain; charset=utf-8"

/* One HTTP response as produced by a route handler. The body pointer is
 * borrowed; it must stay valid until the connection has emitted it. */
typedef struct {
    int status;
    const char *contentType;     /* NULL: no Content-Type header */
    const unsigned char *body;
    size_t bodyLength;
    bool closeConnection;        /* force Connection: close */
    bool cors;                   /* add Access-Control-Allow-* headers */
    /* Zero or more complete "Name: value\r\n" lines, or NULL. Used for
     * 101 upgrade headers and Content-Range/Accept-Ranges. */
    const char *extraHeaders;
    /* The handler will answer later through bb_connection_complete. The
     * connection keeps the request and body buffered until then. */
    bool pending;
    /* Opaque handler state handed back to the connection owner: the session
     * behind a 101 upgrade or a held polling request. */
    void *handlerContext;
} BBHTTPResponse;

void bb_response_init(BBHTTPResponse *response);

const char *bb_response_status_text(int status);

/* Write "HTTP/1.1 <status> <text>\r\n", Content-Type, Content-Length,
 * Connection, CORS, extra headers, and the terminating blank line. A 101
 * response omits Content-Length and reports Connection: Upgrade. */
bool bb_response_write_head(const BBHTTPResponse *response,
                            bool keepAlive,
                            char *out,
                            size_t capacity,
                            size_t *outLength);

/* Bounded JSON text writer. Nothing is allocated; overflow is sticky. */
typedef struct {
    char *out;
    size_t capacity;
    size_t length;
    bool overflow;
} BBJSONWriter;

void bb_json_writer_init(BBJSONWriter *writer, char *out, size_t capacity);
bool bb_json_write_raw(BBJSONWriter *writer, const char *text);
bool bb_json_write_bytes(BBJSONWriter *writer, const char *bytes, size_t length);
/* Quoted and escaped; a NULL value writes null. */
bool bb_json_write_string(BBJSONWriter *writer, const char *value);
bool bb_json_write_bool(BBJSONWriter *writer, bool value);
bool bb_json_write_null(BBJSONWriter *writer);
/* Decimal formatting without 64-bit division helpers. */
bool bb_json_write_unsigned(BBJSONWriter *writer, uint64_t value);
bool bb_json_write_signed(BBJSONWriter *writer, int64_t value);
/* Write "key": with escaping; the caller then writes the value. */
bool bb_json_write_key(BBJSONWriter *writer, const char *key);
/* NUL-terminates and returns false if any write overflowed. */
bool bb_json_writer_finish(BBJSONWriter *writer, size_t *outLength);

/* {"status":200,"message":<message>,"data":<dataJSON>} */
bool bb_response_success_envelope(char *out,
                                  size_t capacity,
                                  size_t *outLength,
                                  const char *message,
                                  const char *dataJSON);

/* General envelope from bridge reply fields. dataJSON, metadataJSON, and
 * errorJSON are raw JSON (already validated by the caller) or NULL; data is
 * emitted as null when absent and there is no error. */
bool bb_response_envelope(char *out,
                          size_t capacity,
                          size_t *outLength,
                          int status,
                          const char *message,
                          const char *dataJSON, size_t dataLength,
                          const char *metadataJSON, size_t metadataLength,
                          const char *errorJSON, size_t errorLength);

/* {"status":<status>,"message":<message>,"error":{"type":<type>,
 *  "message":<detail>}} */
bool bb_response_error_envelope(char *out,
                                size_t capacity,
                                size_t *outLength,
                                int status,
                                const char *message,
                                const char *type,
                                const char *detail);

#ifdef __cplusplus
}
#endif

#endif
