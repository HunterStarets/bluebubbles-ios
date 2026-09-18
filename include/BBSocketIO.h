#ifndef BB_SOCKET_IO_H
#define BB_SOCKET_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BBEIOPacketOpen = 0,
    BBEIOPacketClose = 1,
    BBEIOPacketPing = 2,
    BBEIOPacketPong = 3,
    BBEIOPacketMessage = 4,
    BBEIOPacketUpgrade = 5,
    BBEIOPacketNoop = 6
} BBEIOPacketType;

typedef enum {
    BBSIOPacketNone = -1,
    BBSIOPacketConnect = 0,
    BBSIOPacketDisconnect = 1,
    BBSIOPacketEvent = 2,
    BBSIOPacketAck = 3,
    BBSIOPacketConnectError = 4
} BBSIOPacketType;

typedef struct {
    BBEIOPacketType engineType;
    BBSIOPacketType socketType;
    bool hasSocketPacket;
    bool hasNamespace;
    bool hasAckID;
    uint64_t ackID;
    size_t namespaceOffset;
    size_t namespaceLength;
    size_t jsonOffset;
    size_t jsonLength;
    size_t eventNameOffset;
    size_t eventNameLength;
} BBSIOPacket;

/* Parse one complete Engine.IO text packet and, for Engine.IO message packets,
 * its Socket.IO v5 prefix. Binary Socket.IO packets are deliberately rejected
 * until attachment placeholder handling is implemented. JSON offsets refer to
 * the caller-owned packet buffer. */
bool bb_sio_parse_packet(const unsigned char *packet,
                         size_t packetLength,
                         BBSIOPacket *out);

/* Compare an EVENT's first JSON-array string with an unescaped ASCII event
 * name. JSON escapes in the wire name are decoded for the comparison. */
bool bb_sio_event_name_equals(const unsigned char *packet,
                              size_t packetLength,
                              const BBSIOPacket *parsed,
                              const char *eventName);

/* Iterate an Engine.IO polling payload. Packets are separated by ASCII record
 * separator (0x1e). Set *cursor to zero before the first call. Empty packets
 * are rejected. */
bool bb_eio_next_payload_packet(const unsigned char *payload,
                                size_t payloadLength,
                                size_t *cursor,
                                size_t *packetOffset,
                                size_t *packetLength);

#ifdef __cplusplus
}
#endif

#endif
