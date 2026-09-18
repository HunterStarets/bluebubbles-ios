#include "BBSocketIO.h"

#include <stdint.h>

static void bb_sio_memory_set(void *destination, unsigned char value,
                              size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static size_t bb_sio_string_length(const char *value)
{
    size_t length = 0;
    if (!value) return 0;
    while (value[length]) length++;
    return length;
}

static int bb_sio_hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/* Locate a JSON string's closing quote. The offset/length exclude quotes but
 * retain escapes so the original packet remains the only storage owner. */
static bool bb_sio_json_string(const unsigned char *packet,
                               size_t packetLength,
                               size_t quoteOffset,
                               size_t *valueOffset,
                               size_t *valueLength,
                               size_t *afterQuote)
{
    if (!packet || quoteOffset >= packetLength || packet[quoteOffset] != '"')
        return false;
    size_t cursor = quoteOffset + 1;
    size_t start = cursor;
    while (cursor < packetLength) {
        unsigned char value = packet[cursor];
        if (value == '"') {
            if (valueOffset) *valueOffset = start;
            if (valueLength) *valueLength = cursor - start;
            if (afterQuote) *afterQuote = cursor + 1;
            return true;
        }
        if (value < 0x20) return false;
        if (value == '\\') {
            cursor++;
            if (cursor >= packetLength) return false;
            unsigned char escaped = packet[cursor];
            if (escaped == 'u') {
                if (cursor + 4 >= packetLength) return false;
                for (size_t i = 1; i <= 4; i++) {
                    if (bb_sio_hex_value(packet[cursor + i]) < 0) return false;
                }
                cursor += 4;
            } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                       escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                       escaped != 'r' && escaped != 't') {
                return false;
            }
        }
        cursor++;
    }
    return false;
}

static bool bb_sio_parse_event_name(const unsigned char *packet,
                                    size_t packetLength,
                                    BBSIOPacket *out)
{
    if (!packet || !out || out->jsonLength < 3 ||
        packet[out->jsonOffset] != '[') return false;
    size_t cursor = out->jsonOffset + 1;
    while (cursor < packetLength &&
           (packet[cursor] == ' ' || packet[cursor] == '\t' ||
            packet[cursor] == '\r' || packet[cursor] == '\n')) cursor++;
    size_t afterQuote = 0;
    if (!bb_sio_json_string(packet, packetLength, cursor,
                            &out->eventNameOffset, &out->eventNameLength,
                            &afterQuote)) return false;
    cursor = afterQuote;
    while (cursor < packetLength &&
           (packet[cursor] == ' ' || packet[cursor] == '\t' ||
            packet[cursor] == '\r' || packet[cursor] == '\n')) cursor++;
    return cursor < packetLength &&
           (packet[cursor] == ',' || packet[cursor] == ']');
}

bool bb_sio_parse_packet(const unsigned char *packet,
                         size_t packetLength,
                         BBSIOPacket *out)
{
    if (!out) return false;
    bb_sio_memory_set(out, 0, sizeof(*out));
    out->socketType = BBSIOPacketNone;
    if (!packet || packetLength == 0 || packet[0] < '0' || packet[0] > '6')
        return false;
    out->engineType = (BBEIOPacketType)(packet[0] - '0');
    if (out->engineType != BBEIOPacketMessage) {
        /* Engine control packet data is intentionally opaque. */
        return true;
    }
    if (packetLength < 2 || packet[1] < '0' || packet[1] > '6') return false;
    unsigned char socketType = (unsigned char)(packet[1] - '0');
    if (socketType > (unsigned char)BBSIOPacketConnectError) return false;
    out->hasSocketPacket = true;
    out->socketType = (BBSIOPacketType)socketType;

    size_t cursor = 2;
    if (cursor < packetLength && packet[cursor] == '/') {
        size_t start = cursor;
        while (cursor < packetLength && packet[cursor] != ',') {
            if (packet[cursor] < 0x21 || packet[cursor] == 0x7f) return false;
            cursor++;
        }
        /* A bare "/" is the root namespace spelled explicitly. */
        if (cursor >= packetLength) return false;
        out->hasNamespace = true;
        out->namespaceOffset = start;
        out->namespaceLength = cursor - start;
        cursor++;
    }

    if (cursor < packetLength && packet[cursor] >= '0' && packet[cursor] <= '9') {
        uint64_t ackID = 0;
        out->hasAckID = true;
        while (cursor < packetLength && packet[cursor] >= '0' && packet[cursor] <= '9') {
            unsigned int digit = (unsigned int)(packet[cursor] - '0');
            if (ackID > UINT64_C(1844674407370955161) ||
                (ackID == UINT64_C(1844674407370955161) && digit > 5))
                return false;
            ackID = ackID * 10 + digit;
            cursor++;
        }
        out->ackID = ackID;
    }

    out->jsonOffset = cursor;
    out->jsonLength = packetLength - cursor;
    switch (out->socketType) {
        case BBSIOPacketConnect:
            if (out->hasAckID) return false;
            return out->jsonLength == 0 || packet[cursor] == '{';
        case BBSIOPacketDisconnect:
            return !out->hasAckID && out->jsonLength == 0;
        case BBSIOPacketEvent:
            return bb_sio_parse_event_name(packet, packetLength, out);
        case BBSIOPacketAck:
            return out->hasAckID && out->jsonLength >= 2 &&
                   packet[cursor] == '[' && packet[packetLength - 1] == ']';
        case BBSIOPacketConnectError:
            return !out->hasAckID && out->jsonLength >= 2 &&
                   packet[cursor] == '{' && packet[packetLength - 1] == '}';
        default:
            return false;
    }
}

static bool bb_sio_next_event_byte(const unsigned char *packet,
                                   size_t end,
                                   size_t *cursor,
                                   unsigned char *out)
{
    if (!packet || !cursor || !out || *cursor >= end) return false;
    unsigned char value = packet[*cursor];
    *cursor += 1;
    if (value != '\\') {
        *out = value;
        return value >= 0x20;
    }
    if (*cursor >= end) return false;
    value = packet[*cursor];
    *cursor += 1;
    switch (value) {
        case '"': case '\\': case '/': *out = value; return true;
        case 'b': *out = '\b'; return true;
        case 'f': *out = '\f'; return true;
        case 'n': *out = '\n'; return true;
        case 'r': *out = '\r'; return true;
        case 't': *out = '\t'; return true;
        case 'u': {
            if (*cursor + 4 > end) return false;
            unsigned int code = 0;
            for (size_t i = 0; i < 4; i++) {
                int digit = bb_sio_hex_value(packet[*cursor + i]);
                if (digit < 0) return false;
                code = code * 16 + (unsigned int)digit;
            }
            *cursor += 4;
            if (code > 0x7f || code == 0) return false;
            *out = (unsigned char)code;
            return true;
        }
        default: return false;
    }
}

bool bb_sio_event_name_equals(const unsigned char *packet,
                              size_t packetLength,
                              const BBSIOPacket *parsed,
                              const char *eventName)
{
    if (!packet || !parsed || !eventName ||
        parsed->socketType != BBSIOPacketEvent ||
        parsed->eventNameOffset > packetLength ||
        parsed->eventNameLength > packetLength - parsed->eventNameOffset)
        return false;
    size_t wantedLength = bb_sio_string_length(eventName);
    size_t wantedCursor = 0;
    size_t cursor = parsed->eventNameOffset;
    size_t end = cursor + parsed->eventNameLength;
    while (cursor < end) {
        unsigned char value = 0;
        if (!bb_sio_next_event_byte(packet, end, &cursor, &value) ||
            wantedCursor >= wantedLength ||
            value != (unsigned char)eventName[wantedCursor]) return false;
        wantedCursor++;
    }
    return wantedCursor == wantedLength;
}

bool bb_eio_next_payload_packet(const unsigned char *payload,
                                size_t payloadLength,
                                size_t *cursor,
                                size_t *packetOffset,
                                size_t *packetLength)
{
    if (!payload || !cursor || !packetOffset || !packetLength ||
        *cursor >= payloadLength) return false;
    size_t start = *cursor;
    size_t end = start;
    while (end < payloadLength && payload[end] != 0x1e) end++;
    if (end == start) return false;
    *packetOffset = start;
    *packetLength = end - start;
    *cursor = end < payloadLength ? end + 1 : end;
    return true;
}
