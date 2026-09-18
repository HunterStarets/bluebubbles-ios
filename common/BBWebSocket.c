#include "BBWebSocket.h"

/* Volatile loops keep the older armv7 Clang from lowering these into libc
 * calls the SpringBoard target must not import. */
static void bb_ws_memory_set(void *destination, unsigned char value, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static size_t bb_ws_string_length(const char *value)
{
    const volatile char *bytes = (const volatile char *)value;
    size_t length = 0;
    if (!value) return 0;
    while (bytes[length]) length++;
    return length;
}

static uint16_t bb_ws_read_u16(const unsigned char *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint64_t bb_ws_read_u64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

static bool bb_ws_is_valid_opcode(unsigned char opcode)
{
    return opcode == 0x0 || opcode == 0x1 || opcode == 0x2 ||
           opcode == 0x8 || opcode == 0x9 || opcode == 0xA;
}

static bool bb_ws_is_control_opcode(unsigned char opcode)
{
    return opcode >= 0x8;
}

BBWSFrameParseResult bb_ws_parse_client_frame(const unsigned char *buffer,
                                              size_t bufferLength,
                                              BBWSFrame *out)
{
    if (!out) return BBWSFrameInvalid;
    bb_ws_memory_set(out, 0, sizeof(*out));
    if (!buffer || bufferLength < 2) return BBWSFrameNeedMore;

    unsigned char first = buffer[0];
    unsigned char second = buffer[1];
    unsigned char opcode = (unsigned char)(first & 0x0F);
    bool fin = (first & 0x80) != 0;
    bool masked = (second & 0x80) != 0;
    unsigned char lengthCode = (unsigned char)(second & 0x7F);

    /* RSV1/2/3 require an extension that is never negotiated. Incoming
     * client frames must also carry a masking key. */
    if ((first & 0x70) != 0 || !masked || !bb_ws_is_valid_opcode(opcode)) {
        return BBWSFrameInvalid;
    }
    if (bb_ws_is_control_opcode(opcode) && (!fin || lengthCode > 125)) {
        return BBWSFrameInvalid;
    }

    size_t extendedLength = 0;
    if (lengthCode == 126) {
        extendedLength = 2;
    } else if (lengthCode == 127) {
        extendedLength = 8;
    }
    size_t headerWithoutMask = 2 + extendedLength;
    size_t headerLength = headerWithoutMask + 4;
    if (bufferLength < headerLength) return BBWSFrameNeedMore;

    uint64_t payloadLength = lengthCode;
    if (lengthCode == 126) {
        payloadLength = bb_ws_read_u16(buffer + 2);
        if (payloadLength < 126) return BBWSFrameInvalid;
    } else if (lengthCode == 127) {
        /* RFC 6455 reserves the high bit of the 64-bit length. */
        if ((buffer[2] & 0x80) != 0) return BBWSFrameInvalid;
        payloadLength = bb_ws_read_u64(buffer + 2);
        if (payloadLength <= 65535) return BBWSFrameInvalid;
    }
    if (payloadLength > BB_WS_MAX_PAYLOAD_BYTES) return BBWSFrameInvalid;
    if (payloadLength > (uint64_t)(SIZE_MAX - headerLength)) {
        return BBWSFrameInvalid;
    }

    size_t frameLength = headerLength + (size_t)payloadLength;
    if (bufferLength < frameLength) return BBWSFrameNeedMore;

    out->fin = fin;
    out->opcode = opcode;
    out->masked = masked;
    out->headerLength = headerLength;
    out->payloadLength = payloadLength;
    out->frameLength = frameLength;
    for (size_t i = 0; i < 4; i++) {
        out->maskingKey[i] = buffer[headerWithoutMask + i];
    }
    out->payload = buffer + headerLength;
    return BBWSFrameReady;
}

bool bb_ws_copy_unmasked_payload(const BBWSFrame *frame,
                                 unsigned char *out,
                                 size_t outCapacity,
                                 size_t *outLength)
{
    if (!frame || !frame->masked || frame->payloadLength > SIZE_MAX) return false;
    size_t payloadLength = (size_t)frame->payloadLength;
    if (payloadLength > outCapacity || (payloadLength != 0 && !out)) return false;
    volatile unsigned char *destination = (volatile unsigned char *)out;
    for (size_t i = 0; i < payloadLength; i++) {
        destination[i] = (unsigned char)(frame->payload[i] ^ frame->maskingKey[i % 4]);
    }
    if (outLength) *outLength = payloadLength;
    return true;
}

static bool bb_ws_write_byte(unsigned char value, unsigned char *out,
                             size_t capacity, size_t *cursor)
{
    if (!out || !cursor || *cursor >= capacity) return false;
    out[*cursor] = value;
    *cursor += 1;
    return true;
}

bool bb_ws_write_server_frame_header(unsigned char opcode,
                                     size_t payloadLength,
                                     unsigned char *out,
                                     size_t outCapacity,
                                     size_t *outLength)
{
    if (!out || payloadLength > BB_WS_MAX_PAYLOAD_BYTES) return false;
    if (!bb_ws_is_valid_opcode(opcode)) return false;
    if (bb_ws_is_control_opcode(opcode) && payloadLength > 125) return false;

    size_t cursor = 0;
    if (!bb_ws_write_byte((unsigned char)(0x80 | opcode), out, outCapacity, &cursor)) {
        return false;
    }
    if (payloadLength < 126) {
        if (!bb_ws_write_byte((unsigned char)payloadLength, out, outCapacity, &cursor)) {
            return false;
        }
    } else if (payloadLength <= 65535) {
        if (!bb_ws_write_byte(126, out, outCapacity, &cursor) ||
            !bb_ws_write_byte((unsigned char)(payloadLength >> 8), out, outCapacity, &cursor) ||
            !bb_ws_write_byte((unsigned char)payloadLength, out, outCapacity, &cursor)) {
            return false;
        }
    } else {
        if (!bb_ws_write_byte(127, out, outCapacity, &cursor)) return false;
        uint64_t length = payloadLength;
        for (int shift = 56; shift >= 0; shift -= 8) {
            if (!bb_ws_write_byte((unsigned char)(length >> shift), out,
                                  outCapacity, &cursor)) return false;
        }
    }
    if (outLength) *outLength = cursor;
    return true;
}

bool bb_ws_build_server_frame(unsigned char opcode,
                              const unsigned char *payload,
                              size_t payloadLength,
                              unsigned char *out,
                              size_t outCapacity,
                              size_t *outLength)
{
    size_t headerLength = 0;
    if (payloadLength != 0 && !payload) return false;
    if (!bb_ws_write_server_frame_header(opcode, payloadLength, out, outCapacity,
                                         &headerLength)) return false;
    if (payloadLength > outCapacity - headerLength) return false;
    volatile unsigned char *destination = (volatile unsigned char *)out + headerLength;
    for (size_t i = 0; i < payloadLength; i++) destination[i] = payload[i];
    if (outLength) *outLength = headerLength + payloadLength;
    return true;
}

/* ---- Sec-WebSocket-Accept: SHA-1 over key + RFC 6455 GUID, base64 ---- */

#define BB_WS_SHA1_MAX_INPUT_BYTES 256U

static uint32_t bb_ws_rotate_left(uint32_t value, unsigned int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

/* Bounded single-buffer SHA-1 (FIPS 180-4). Inputs are at most 256 bytes,
 * so the padded message is at most 320 bytes (five blocks). */
static bool bb_ws_sha1(const unsigned char *input, size_t length,
                       unsigned char digest[20])
{
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;
    unsigned char padded[BB_WS_SHA1_MAX_INPUT_BYTES + 64];
    uint32_t w[80];
    size_t paddedLength;
    uint64_t bits;

    if ((length && !input) || length > BB_WS_SHA1_MAX_INPUT_BYTES) return false;
    /* Smallest multiple of 64 that holds the message, 0x80, and the length. */
    paddedLength = ((length + 8) / 64 + 1) * 64;
    for (size_t i = 0; i < length; i++) padded[i] = input[i];
    padded[length] = 0x80;
    for (size_t i = length + 1; i < paddedLength - 8; i++) padded[i] = 0;
    bits = (uint64_t)length * 8u;
    for (int i = 0; i < 8; i++) {
        padded[paddedLength - 8 + i] = (unsigned char)(bits >> (56 - 8 * i));
    }

    for (size_t offset = 0; offset < paddedLength; offset += 64) {
        const unsigned char *block = padded + offset;
        for (size_t i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
        }
        for (size_t i = 16; i < 80; i++) {
            w[i] = bb_ws_rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (size_t i = 0; i < 80; i++) {
            uint32_t f;
            uint32_t k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = bb_ws_rotate_left(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = bb_ws_rotate_left(b, 30);
            b = a;
            a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint32_t words[5] = { h0, h1, h2, h3, h4 };
    for (size_t i = 0; i < 5; i++) {
        digest[i * 4] = (unsigned char)(words[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(words[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(words[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)words[i];
    }
    bb_ws_memory_set(padded, 0, sizeof(padded));
    bb_ws_memory_set(w, 0, sizeof(w));
    return true;
}

static const char bb_ws_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool bb_ws_accept_key(const char *clientKey,
                      char *out,
                      size_t outCapacity)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char material[128];
    unsigned char digest[20];
    size_t keyLength = bb_ws_string_length(clientKey);
    size_t guidLength = sizeof(guid) - 1;
    size_t cursor = 0;

    if (!clientKey || !out || outCapacity < BB_WS_ACCEPT_KEY_BYTES) return false;
    /* A valid key is 16 random bytes in base64: exactly 24 characters. */
    if (keyLength != 24) return false;
    for (size_t i = 0; i < keyLength; i++) {
        unsigned char byte = (unsigned char)clientKey[i];
        bool base64 = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                      (byte >= '0' && byte <= '9') || byte == '+' || byte == '/' ||
                      (byte == '=' && i >= 22);
        if (!base64) return false;
        material[cursor++] = byte;
    }
    for (size_t i = 0; i < guidLength; i++) material[cursor++] = (unsigned char)guid[i];
    if (!bb_ws_sha1(material, cursor, digest)) return false;

    size_t written = 0;
    for (size_t i = 0; i < 20; i += 3) {
        uint32_t chunk = (uint32_t)digest[i] << 16;
        size_t remaining = 20 - i;
        if (remaining > 1) chunk |= (uint32_t)digest[i + 1] << 8;
        if (remaining > 2) chunk |= (uint32_t)digest[i + 2];
        out[written++] = bb_ws_base64_alphabet[(chunk >> 18) & 0x3f];
        out[written++] = bb_ws_base64_alphabet[(chunk >> 12) & 0x3f];
        out[written++] = remaining > 1 ? bb_ws_base64_alphabet[(chunk >> 6) & 0x3f] : '=';
        out[written++] = remaining > 2 ? bb_ws_base64_alphabet[chunk & 0x3f] : '=';
    }
    out[written] = '\0';
    return written == BB_WS_ACCEPT_KEY_BYTES - 1;
}
