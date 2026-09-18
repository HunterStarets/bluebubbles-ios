#ifndef BB_WEBSOCKET_H
#define BB_WEBSOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RFC 6455 framing for the Socket.IO transport. Engine.IO advertises a
 * 1,000,000-byte maxPayload; this bound covers it with framing overhead. */
#define BB_WS_MAX_PAYLOAD_BYTES (1024U * 1024U)
#define BB_WS_MAX_HEADER_BYTES 10U
/* 28 base64 characters plus a terminator. */
#define BB_WS_ACCEPT_KEY_BYTES 29U

typedef enum {
    BBWSFrameNeedMore = 0,
    BBWSFrameReady = 1,
    BBWSFrameInvalid = -1
} BBWSFrameParseResult;

typedef struct {
    bool fin;
    unsigned char opcode;
    bool masked;
    size_t headerLength;
    uint64_t payloadLength;
    size_t frameLength;
    unsigned char maskingKey[4];
    const unsigned char *payload;
} BBWSFrame;

/* Parse one client-to-server frame. Client frames must be masked. */
BBWSFrameParseResult bb_ws_parse_client_frame(const unsigned char *buffer,
                                              size_t bufferLength,
                                              BBWSFrame *out);

/* Copy a parsed masked payload into caller-owned storage and unmask it. */
bool bb_ws_copy_unmasked_payload(const BBWSFrame *frame,
                                 unsigned char *out,
                                 size_t outCapacity,
                                 size_t *outLength);

/* Build one unmasked server-to-client frame. The result is always FIN. */
bool bb_ws_build_server_frame(unsigned char opcode,
                              const unsigned char *payload,
                              size_t payloadLength,
                              unsigned char *out,
                              size_t outCapacity,
                              size_t *outLength);

/* Write only the FIN frame header for a payload the caller has already
 * placed immediately after it. */
bool bb_ws_write_server_frame_header(unsigned char opcode,
                                     size_t payloadLength,
                                     unsigned char *out,
                                     size_t outCapacity,
                                     size_t *outLength);

/* Compute Sec-WebSocket-Accept for a Sec-WebSocket-Key header value. */
bool bb_ws_accept_key(const char *clientKey,
                      char *out,
                      size_t outCapacity);

#ifdef __cplusplus
}
#endif

#endif
