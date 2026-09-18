#ifndef BB_ENGINE_IO_H
#define BB_ENGINE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "BBSocketIO.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Engine.IO v4 / Socket.IO v5 session state for one client. The module
 * allocates nothing and owns no timers: the caller supplies buffers,
 * delivers bytes, and drives time through bb_eio_session_tick. Outbound
 * bytes leave through callbacks (WebSocket) or a drainable queue (polling). */

#define BB_EIO_SID_BYTES 48U
#define BB_EIO_PING_INTERVAL_MS UINT64_C(60000)
#define BB_EIO_PING_TIMEOUT_MS UINT64_C(120000)
#define BB_EIO_MAX_PAYLOAD_BYTES 1000000U
/* Open packet plus sid must fit here. */
#define BB_EIO_OPEN_PACKET_BYTES 192U
#define BB_EIO_RECORD_SEPARATOR 0x1e

typedef enum {
    BBEIOTransportWebSocket = 0,
    BBEIOTransportPolling
} BBEIOTransport;

typedef enum {
    BBEIOSessionStateUnused = 0,
    BBEIOSessionStateOpening,     /* open packet sent; awaiting 40 */
    BBEIOSessionStateConnected,   /* namespace connected */
    BBEIOSessionStateClosed
} BBEIOSessionState;

typedef enum {
    BBEIOCloseNone = 0,
    BBEIOClosePeerDisconnect,     /* 41 or Engine.IO close */
    BBEIOClosePeerFrame,          /* WebSocket close frame */
    BBEIOClosePingTimeout,
    BBEIOCloseProtocolError,
    BBEIOCloseBufferExhausted,
    BBEIOCloseTransportFailed,
    BBEIOCloseLocal
} BBEIOCloseReason;

typedef struct BBEIOSession BBEIOSession;

typedef struct {
    /* A Socket.IO EVENT arrived. packet spans the whole Engine.IO packet;
     * parsed carries JSON/event-name offsets and the optional ACK id. */
    void (*event)(void *context, BBEIOSession *session,
                  const unsigned char *packet, size_t packetLength,
                  const BBSIOPacket *parsed);
    /* WebSocket sessions: framed bytes to write to the socket. */
    bool (*sendFrame)(void *context, BBEIOSession *session,
                      const unsigned char *bytes, size_t length);
    /* Polling sessions: the queue became non-empty, so a held GET can be
     * completed with bb_eio_session_drain. */
    void (*pollingReady)(void *context, BBEIOSession *session);
    /* The session is finished; the owner releases its slot and socket. */
    void (*closed)(void *context, BBEIOSession *session,
                   BBEIOCloseReason reason);
    void *context;
} BBEIOCallbacks;

struct BBEIOSession {
    BBEIOSessionState state;
    BBEIOTransport transport;
    char sid[BB_EIO_SID_BYTES];
    char socketSid[BB_EIO_SID_BYTES];
    /* WebSocket receive: partial frames, then the reassembled message. */
    unsigned char *frames;
    size_t framesCapacity;
    size_t framesLength;
    unsigned char *message;
    size_t messageCapacity;
    size_t messageLength;
    bool messageInProgress;
    /* Outbound: frame scratch (WebSocket) or packet queue (polling). */
    unsigned char *outbound;
    size_t outboundCapacity;
    size_t outboundLength;
    /* Heartbeat, in the caller's millisecond clock. */
    uint64_t lastPingSentAt;
    uint64_t lastActivityAt;
    bool awaitingPong;
    bool closeFrameSent;
    /* Owner state for a held polling GET; never touched by this module. */
    void *pollingWaiter;
    BBEIOCallbacks callbacks;
    /* Count-only diagnostics. */
    unsigned long packetsReceived;
    unsigned long eventsReceived;
    unsigned long packetsSent;
    unsigned long acksSent;
    unsigned long pingsSent;
    unsigned long pongsReceived;
};

/* Write the Engine.IO open packet for sid into out. */
bool bb_eio_write_open_packet(const char *sid, char *out, size_t capacity,
                              size_t *outLength);

/* Attach caller-owned buffers to a slot once. They survive session reuse.
 * The frames and message buffers are only needed for WebSocket sessions. */
void bb_eio_session_set_buffers(BBEIOSession *session,
                                unsigned char *frames, size_t framesCapacity,
                                unsigned char *message, size_t messageCapacity,
                                unsigned char *outbound, size_t outboundCapacity);

/* Start using a slot for a new client. nowMs starts the heartbeat. */
bool bb_eio_session_init(BBEIOSession *session,
                         BBEIOTransport transport,
                         const char *sid,
                         const char *socketSid,
                         const BBEIOCallbacks *callbacks,
                         uint64_t nowMs);

/* Send the open packet through the transport (WebSocket sessions call this
 * right after the 101; polling sessions return it in the initial GET). */
bool bb_eio_session_start(BBEIOSession *session);

/* The receive functions return false only when the input was rejected
 * (protocol error, buffer exhaustion, transport failure) and the session
 * was closed for it. A clean peer disconnect returns true; the closed
 * callback reports it. */

/* Bytes from an upgraded WebSocket connection. */
bool bb_eio_session_receive_bytes(BBEIOSession *session,
                                  const unsigned char *bytes, size_t length,
                                  uint64_t nowMs);

/* A polling POST body: record-separated Engine.IO packets. */
bool bb_eio_session_receive_payload(BBEIOSession *session,
                                    const unsigned char *payload,
                                    size_t payloadLength,
                                    uint64_t nowMs);

/* One complete Engine.IO packet from either transport. */
bool bb_eio_session_receive_packet(BBEIOSession *session,
                                   const unsigned char *packet,
                                   size_t packetLength,
                                   uint64_t nowMs);

bool bb_eio_session_is_live(const BBEIOSession *session);

/* Send or queue one Engine.IO packet (already prefixed). */
bool bb_eio_session_send_packet(BBEIOSession *session,
                                const unsigned char *packet,
                                size_t packetLength);

/* 43<ackID>[<envelopeJSON>] */
bool bb_eio_session_ack(BBEIOSession *session, uint64_t ackID,
                        const char *envelopeJSON, size_t envelopeLength);

/* 42["<eventName>",<payloadJSON>]; payload is a raw object, not an
 * envelope, matching BlueBubbles push events. */
bool bb_eio_session_emit(BBEIOSession *session, const char *eventName,
                         const char *payloadJSON, size_t payloadLength);

/* Drive the heartbeat. Sends a ping when due and closes on timeout. */
void bb_eio_session_tick(BBEIOSession *session, uint64_t nowMs);

/* Polling: copy queued packets (record separated) and empty the queue.
 * Returns false when nothing is queued. */
bool bb_eio_session_drain(BBEIOSession *session, unsigned char *out,
                          size_t capacity, size_t *outLength);
size_t bb_eio_session_queued_bytes(const BBEIOSession *session);

void bb_eio_session_close(BBEIOSession *session, BBEIOCloseReason reason);

/* Bounded session table. The owner attaches buffers to every slot with
 * bb_eio_session_set_buffers; the table only tracks which slots are used. */
typedef struct {
    BBEIOSession *sessions;
    size_t capacity;
    /* Produce a unique session id (letters, digits, '-', '_'), at most
     * BB_EIO_SID_BYTES - 1 characters. */
    bool (*generateSid)(void *context, char *out, size_t capacity);
    void *context;
} BBEIOSessionTable;

BBEIOSession *bb_eio_table_find(const BBEIOSessionTable *table,
                                const char *sid);
/* Claim a free slot, generate its Engine.IO and Socket.IO ids, and
 * initialize it. NULL when the table is full or id generation fails. */
BBEIOSession *bb_eio_table_take(const BBEIOSessionTable *table,
                                BBEIOTransport transport,
                                const BBEIOCallbacks *callbacks,
                                uint64_t nowMs);
size_t bb_eio_table_active_count(const BBEIOSessionTable *table);
/* Return a closed session's slot to the table. */
void bb_eio_table_release(BBEIOSession *session);

#ifdef __cplusplus
}
#endif

#endif
