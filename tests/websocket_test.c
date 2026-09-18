#include "BBWebSocket.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_masked_client_frames(void)
{
    /* "list" masked with the synthetic key 01 02 03 04. */
    const unsigned char frame[] = {
        0x81, 0x84, 0x01, 0x02, 0x03, 0x04,
        0x6D, 0x6B, 0x70, 0x70
    };
    BBWSFrame parsed;
    CHECK(bb_ws_parse_client_frame(frame, sizeof(frame), &parsed) == BBWSFrameReady);
    CHECK(parsed.fin && parsed.opcode == 0x1 && parsed.masked);
    CHECK(parsed.headerLength == 6 && parsed.frameLength == sizeof(frame));
    CHECK(parsed.payloadLength == 4);

    unsigned char payload[8] = {0};
    size_t payloadLength = 0;
    CHECK(bb_ws_copy_unmasked_payload(&parsed, payload, sizeof(payload), &payloadLength));
    CHECK(payloadLength == 4 && memcmp(payload, "list", 4) == 0);

    CHECK(bb_ws_parse_client_frame(frame, 5, &parsed) == BBWSFrameNeedMore);
    CHECK(bb_ws_parse_client_frame(frame, 9, &parsed) == BBWSFrameNeedMore);
}

static void test_rejections(void)
{
    const unsigned char unmasked[] = { 0x81, 0x04, 'l', 'i', 's', 't' };
    const unsigned char reserved[] = { 0xC1, 0x80, 0, 0, 0, 0 };
    const unsigned char controlFragment[] = { 0x09, 0x80, 0, 0, 0, 0 };
    const unsigned char badOpcode[] = { 0x8B, 0x80, 0, 0, 0, 0 };
    const unsigned char bad64[] = {
        0x81, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    const unsigned char nonMinimal[] = {
        0x81, 0xFE, 0, 1, 0, 0, 0, 0, 'x'
    };
    BBWSFrame parsed;
    CHECK(bb_ws_parse_client_frame(unmasked, sizeof(unmasked), &parsed) == BBWSFrameInvalid);
    CHECK(bb_ws_parse_client_frame(reserved, sizeof(reserved), &parsed) == BBWSFrameInvalid);
    CHECK(bb_ws_parse_client_frame(controlFragment, sizeof(controlFragment), &parsed) == BBWSFrameInvalid);
    CHECK(bb_ws_parse_client_frame(badOpcode, sizeof(badOpcode), &parsed) == BBWSFrameInvalid);
    CHECK(bb_ws_parse_client_frame(bad64, sizeof(bad64), &parsed) == BBWSFrameInvalid);
    CHECK(bb_ws_parse_client_frame(nonMinimal, sizeof(nonMinimal), &parsed) == BBWSFrameInvalid);
}

static void test_server_frames(void)
{
    const char shortPayload[] = "[],/list";
    unsigned char frame[2048] = {0};
    size_t frameLength = 0;
    CHECK(bb_ws_build_server_frame(0x1, (const unsigned char *)shortPayload,
                                   strlen(shortPayload), frame, sizeof(frame), &frameLength));
    CHECK(frame[0] == 0x81 && frame[1] == strlen(shortPayload));
    CHECK(frameLength == strlen(shortPayload) + 2);
    CHECK(memcmp(frame + 2, shortPayload, strlen(shortPayload)) == 0);

    unsigned char extendedPayload[126];
    for (size_t i = 0; i < sizeof(extendedPayload); i++) extendedPayload[i] = 'x';
    CHECK(bb_ws_build_server_frame(0x1, extendedPayload, sizeof(extendedPayload),
                                   frame, sizeof(frame), &frameLength));
    CHECK(frame[0] == 0x81 && frame[1] == 126 && frame[2] == 0 && frame[3] == 126);
    CHECK(frameLength == sizeof(extendedPayload) + 4);

    CHECK(!bb_ws_build_server_frame(0x9, extendedPayload, sizeof(extendedPayload),
                                    frame, sizeof(frame), &frameLength));
    CHECK(!bb_ws_build_server_frame(0x3, NULL, 0, frame, sizeof(frame), &frameLength));
}

static void test_extended_client_frame(void)
{
    unsigned char frame[8 + 126];
    unsigned char key[4] = { 0x11, 0x22, 0x33, 0x44 };
    frame[0] = 0x81;
    frame[1] = 0xFE;
    frame[2] = 0;
    frame[3] = 126;
    for (size_t i = 0; i < 4; i++) frame[4 + i] = key[i];
    for (size_t i = 0; i < 126; i++) frame[8 + i] = (unsigned char)('a' ^ key[i % 4]);

    BBWSFrame parsed;
    CHECK(bb_ws_parse_client_frame(frame, sizeof(frame), &parsed) == BBWSFrameReady);
    CHECK(parsed.headerLength == 8 && parsed.payloadLength == 126);
    unsigned char payload[126];
    size_t payloadLength = 0;
    CHECK(bb_ws_copy_unmasked_payload(&parsed, payload, sizeof(payload), &payloadLength));
    CHECK(payloadLength == sizeof(payload));
    for (size_t i = 0; i < payloadLength; i++) CHECK(payload[i] == 'a');
}

static void test_accept_key_and_header_writer(void)
{
    char accept[BB_WS_ACCEPT_KEY_BYTES];
    unsigned char header[BB_WS_MAX_HEADER_BYTES];
    size_t length = 0;

    /* RFC 6455 section 1.3 example. */
    CHECK(bb_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept, sizeof(accept)));
    CHECK(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    /* RFC 6455 section 4.2.2 example. */
    CHECK(bb_ws_accept_key("x3JJHMbDL1EzLkh9GBhXDw==", accept, sizeof(accept)));
    CHECK(strcmp(accept, "HSmrc0sMlYUkAGmm5OPpG2HaGWk=") == 0);
    CHECK(!bb_ws_accept_key("too-short", accept, sizeof(accept)));
    CHECK(!bb_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ=", accept, sizeof(accept)));
    CHECK(!bb_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept, 10));
    CHECK(!bb_ws_accept_key("dGhlIHNhbXBsZSBub2\x01jZQ==", accept, sizeof(accept)));
    CHECK(!bb_ws_accept_key(NULL, accept, sizeof(accept)));

    CHECK(bb_ws_write_server_frame_header(0x1, 5, header, sizeof(header), &length));
    CHECK(length == 2 && header[0] == 0x81 && header[1] == 5);
    CHECK(bb_ws_write_server_frame_header(0x1, 126, header, sizeof(header), &length));
    CHECK(length == 4 && header[1] == 126 && header[2] == 0 && header[3] == 126);
    CHECK(bb_ws_write_server_frame_header(0x1, 70000, header, sizeof(header), &length));
    CHECK(length == 10 && header[1] == 127 && header[8] == 0x11 && header[9] == 0x70);
    CHECK(!bb_ws_write_server_frame_header(0x9, 126, header, sizeof(header), &length));
    CHECK(!bb_ws_write_server_frame_header(0x1, 5, header, 1, &length));
}

int main(void)
{
    test_masked_client_frames();
    test_rejections();
    test_server_frames();
    test_extended_client_frame();
    test_accept_key_and_header_writer();
    if (failures) return 1;
    puts("websocket tests passed: masked frames, validation, server framing, accept key");
    return 0;
}
