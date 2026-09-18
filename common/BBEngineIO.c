#include "BBEngineIO.h"

#include "BBResponse.h"
#include "BBWebSocket.h"

/* Volatile loops keep the older armv7 Clang from lowering these into libc
 * calls the SpringBoard target must not import. */
static void bb_eio_memory_set(void *destination, unsigned char value,
                              size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; i++) bytes[i] = value;
}

static void bb_eio_memory_copy(void *destination, const void *source,
                               size_t length)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;
    for (size_t i = 0; i < length; i++) out[i] = in[i];
}

static size_t bb_eio_string_length(const char *value)
{
    const volatile char *bytes = (const volatile char *)value;
    size_t length = 0;
    if (!value) return 0;
    while (bytes[length]) length++;
    return length;
}

static bool bb_eio_string_equal(const char *left, const char *right)
{
    size_t i = 0;
    if (!left || !right) return false;
    while (left[i] && right[i]) {
        if (left[i] != right[i]) return false;
        i++;
    }
    return left[i] == right[i];
}

static bool bb_eio_sid_valid(const char *sid)
{
    size_t length = bb_eio_string_length(sid);
    if (!length || length >= BB_EIO_SID_BYTES) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)sid[i];
        bool ok = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
        if (!ok) return false;
    }
    return true;
}

static bool bb_eio_session_live(const BBEIOSession *session)
{
    return session && (session->state == BBEIOSessionStateOpening ||
                       session->state == BBEIOSessionStateConnected);
}

bool bb_eio_session_is_live(const BBEIOSession *session)
{
    return bb_eio_session_live(session);
}

bool bb_eio_write_open_packet(const char *sid, char *out, size_t capacity,
                              size_t *outLength)
{
    BBJSONWriter writer;
    if (!bb_eio_sid_valid(sid)) return false;
    bb_json_writer_init(&writer, out, capacity);
    return bb_json_write_raw(&writer, "0{\"sid\":") &&
           bb_json_write_string(&writer, sid) &&
           bb_json_write_raw(&writer, ",\"upgrades\":[],\"pingInterval\":") &&
           bb_json_write_unsigned(&writer, BB_EIO_PING_INTERVAL_MS) &&
           bb_json_write_raw(&writer, ",\"pingTimeout\":") &&
           bb_json_write_unsigned(&writer, BB_EIO_PING_TIMEOUT_MS) &&
           bb_json_write_raw(&writer, ",\"maxPayload\":") &&
           bb_json_write_unsigned(&writer, BB_EIO_MAX_PAYLOAD_BYTES) &&
           bb_json_write_raw(&writer, "}") &&
           bb_json_writer_finish(&writer, outLength);
}

void bb_eio_session_set_buffers(BBEIOSession *session,
                                unsigned char *frames, size_t framesCapacity,
                                unsigned char *message, size_t messageCapacity,
                                unsigned char *outbound, size_t outboundCapacity)
{
    if (!session) return;
    session->frames = frames;
    session->framesCapacity = frames ? framesCapacity : 0;
    session->message = message;
    session->messageCapacity = message ? messageCapacity : 0;
    session->outbound = outbound;
    session->outboundCapacity = outbound ? outboundCapacity : 0;
}

bool bb_eio_session_init(BBEIOSession *session,
                         BBEIOTransport transport,
                         const char *sid,
                         const char *socketSid,
                         const BBEIOCallbacks *callbacks,
                         uint64_t nowMs)
{
    unsigned char *frames;
    size_t framesCapacity;
    unsigned char *message;
    size_t messageCapacity;
    unsigned char *outbound;
    size_t outboundCapacity;
    size_t sidLength;
    size_t socketSidLength;

    if (!session || !callbacks || !callbacks->closed ||
        !bb_eio_sid_valid(sid) || !bb_eio_sid_valid(socketSid)) return false;
    frames = session->frames;
    framesCapacity = session->framesCapacity;
    message = session->message;
    messageCapacity = session->messageCapacity;
    outbound = session->outbound;
    outboundCapacity = session->outboundCapacity;
    if (!outbound || outboundCapacity <= BB_WS_MAX_HEADER_BYTES + BB_EIO_OPEN_PACKET_BYTES)
        return false;
    if (transport == BBEIOTransportWebSocket) {
        if (!callbacks->sendFrame || !callbacks->event) return false;
        if (!frames || !framesCapacity || !message || !messageCapacity) return false;
    } else if (transport == BBEIOTransportPolling) {
        if (!callbacks->event) return false;
    } else {
        return false;
    }

    bb_eio_memory_set(session, 0, sizeof(*session));
    bb_eio_session_set_buffers(session, frames, framesCapacity, message,
                               messageCapacity, outbound, outboundCapacity);
    sidLength = bb_eio_string_length(sid);
    socketSidLength = bb_eio_string_length(socketSid);
    bb_eio_memory_copy(session->sid, sid, sidLength + 1);
    bb_eio_memory_copy(session->socketSid, socketSid, socketSidLength + 1);
    session->transport = transport;
    session->callbacks = *callbacks;
    session->lastPingSentAt = nowMs;
    session->lastActivityAt = nowMs;
    session->state = BBEIOSessionStateOpening;
    return true;
}

/* ---- outbound ------------------------------------------------------- */

/* Staging area inside the outbound buffer where a packet is built before it
 * is sent. WebSocket: a fixed offset that leaves room for the frame header
 * in front. Polling: just above the queued bytes plus one separator, so
 * building never disturbs the queue. */
static unsigned char *bb_eio_session_stage(BBEIOSession *session,
                                           size_t *capacity)
{
    size_t reserved = BB_WS_MAX_HEADER_BYTES;
    if (session->transport == BBEIOTransportPolling) {
        reserved = session->outboundLength + 1;
    }
    if (reserved >= session->outboundCapacity) {
        *capacity = 0;
        return NULL;
    }
    *capacity = session->outboundCapacity - reserved;
    return session->outbound + reserved;
}

/* Send a packet that was built in the staging area returned by
 * bb_eio_session_stage for the current queue state. */
static bool bb_eio_session_send_staged(BBEIOSession *session,
                                       unsigned char *staged,
                                       size_t packetLength)
{
    if (session->transport == BBEIOTransportWebSocket) {
        unsigned char header[BB_WS_MAX_HEADER_BYTES];
        size_t headerLength = 0;
        unsigned char *frame;
        if (!bb_ws_write_server_frame_header(0x1, packetLength, header,
                                             sizeof(header), &headerLength)) {
            bb_eio_session_close(session, BBEIOCloseProtocolError);
            return false;
        }
        frame = staged - headerLength;
        for (size_t i = 0; i < headerLength; i++) frame[i] = header[i];
        if (!session->callbacks.sendFrame(session->callbacks.context, session,
                                          frame, headerLength + packetLength)) {
            bb_eio_session_close(session, BBEIOCloseTransportFailed);
            return false;
        }
        session->packetsSent++;
        return true;
    }
    {
        size_t separator = session->outboundLength ? 1 : 0;
        size_t tail = session->outboundLength + separator;
        bool wasEmpty = session->outboundLength == 0;
        if (packetLength > session->outboundCapacity - tail) {
            bb_eio_session_close(session, BBEIOCloseBufferExhausted);
            return false;
        }
        if (separator) session->outbound[session->outboundLength] = BB_EIO_RECORD_SEPARATOR;
        /* staged == outbound + outboundLength + 1 >= tail, so this forward
         * byte copy only ever moves bytes down or leaves them in place. */
        bb_eio_memory_copy(session->outbound + tail, staged, packetLength);
        session->outboundLength = tail + packetLength;
        session->packetsSent++;
        if (wasEmpty && session->callbacks.pollingReady) {
            session->callbacks.pollingReady(session->callbacks.context, session);
        }
        return true;
    }
}

bool bb_eio_session_send_packet(BBEIOSession *session,
                                const unsigned char *packet,
                                size_t packetLength)
{
    size_t capacity = 0;
    unsigned char *stage;
    if (!bb_eio_session_live(session) || (packetLength && !packet)) return false;
    if (packetLength == 0 || packetLength > BB_EIO_MAX_PAYLOAD_BYTES) return false;
    stage = bb_eio_session_stage(session, &capacity);
    if (!stage || packetLength > capacity) {
        bb_eio_session_close(session, BBEIOCloseBufferExhausted);
        return false;
    }
    bb_eio_memory_copy(stage, packet, packetLength);
    return bb_eio_session_send_staged(session, stage, packetLength);
}

bool bb_eio_session_start(BBEIOSession *session)
{
    char packet[BB_EIO_OPEN_PACKET_BYTES];
    size_t length = 0;
    if (!session || session->state != BBEIOSessionStateOpening) return false;
    if (!bb_eio_write_open_packet(session->sid, packet, sizeof(packet), &length))
        return false;
    return bb_eio_session_send_packet(session, (const unsigned char *)packet,
                                      length);
}

bool bb_eio_session_ack(BBEIOSession *session, uint64_t ackID,
                        const char *envelopeJSON, size_t envelopeLength)
{
    BBJSONWriter writer;
    size_t capacity = 0;
    size_t length = 0;
    unsigned char *stage;
    if (!bb_eio_session_live(session) || session->state != BBEIOSessionStateConnected ||
        !envelopeJSON || !envelopeLength) return false;
    stage = bb_eio_session_stage(session, &capacity);
    if (!stage) {
        bb_eio_session_close(session, BBEIOCloseBufferExhausted);
        return false;
    }
    bb_json_writer_init(&writer, (char *)stage, capacity);
    if (!bb_json_write_raw(&writer, "43") ||
        !bb_json_write_unsigned(&writer, ackID) ||
        !bb_json_write_raw(&writer, "[") ||
        !bb_json_write_bytes(&writer, envelopeJSON, envelopeLength) ||
        !bb_json_write_raw(&writer, "]") ||
        !bb_json_writer_finish(&writer, &length)) {
        bb_eio_session_close(session, BBEIOCloseBufferExhausted);
        return false;
    }
    if (!bb_eio_session_send_staged(session, stage, length)) return false;
    session->acksSent++;
    return true;
}

bool bb_eio_session_emit(BBEIOSession *session, const char *eventName,
                         const char *payloadJSON, size_t payloadLength)
{
    BBJSONWriter writer;
    size_t capacity = 0;
    size_t length = 0;
    unsigned char *stage;
    if (!bb_eio_session_live(session) || session->state != BBEIOSessionStateConnected ||
        !eventName || !eventName[0] || !payloadJSON || !payloadLength) return false;
    stage = bb_eio_session_stage(session, &capacity);
    if (!stage) {
        bb_eio_session_close(session, BBEIOCloseBufferExhausted);
        return false;
    }
    bb_json_writer_init(&writer, (char *)stage, capacity);
    if (!bb_json_write_raw(&writer, "42[") ||
        !bb_json_write_string(&writer, eventName) ||
        !bb_json_write_raw(&writer, ",") ||
        !bb_json_write_bytes(&writer, payloadJSON, payloadLength) ||
        !bb_json_write_raw(&writer, "]") ||
        !bb_json_writer_finish(&writer, &length)) {
        bb_eio_session_close(session, BBEIOCloseBufferExhausted);
        return false;
    }
    return bb_eio_session_send_staged(session, stage, length);
}

/* ---- inbound -------------------------------------------------------- */

static bool bb_eio_session_send_namespace_connect(BBEIOSession *session)
{
    BBJSONWriter writer;
    size_t capacity = 0;
    size_t length = 0;
    unsigned char *stage = bb_eio_session_stage(session, &capacity);
    if (!stage) {
        bb_eio_session_close(session, BBEIOCloseBufferExhausted);
        return false;
    }
    bb_json_writer_init(&writer, (char *)stage, capacity);
    if (!bb_json_write_raw(&writer, "40{\"sid\":") ||
        !bb_json_write_string(&writer, session->socketSid) ||
        !bb_json_write_raw(&writer, "}") ||
        !bb_json_writer_finish(&writer, &length)) {
        bb_eio_session_close(session, BBEIOCloseBufferExhausted);
        return false;
    }
    return bb_eio_session_send_staged(session, stage, length);
}

static bool bb_eio_namespace_is_root(const unsigned char *packet,
                                     const BBSIOPacket *parsed)
{
    if (!parsed->hasNamespace) return true;
    return parsed->namespaceLength == 1 && packet[parsed->namespaceOffset] == '/';
}

bool bb_eio_session_receive_packet(BBEIOSession *session,
                                   const unsigned char *packet,
                                   size_t packetLength,
                                   uint64_t nowMs)
{
    BBSIOPacket parsed;
    if (!bb_eio_session_live(session)) return false;
    if (!packet || !packetLength || packetLength > BB_EIO_MAX_PAYLOAD_BYTES ||
        !bb_sio_parse_packet(packet, packetLength, &parsed)) {
        bb_eio_session_close(session, BBEIOCloseProtocolError);
        return false;
    }
    session->packetsReceived++;
    session->lastActivityAt = nowMs;

    switch (parsed.engineType) {
        case BBEIOPacketPing: {
            /* v4 clients do not ping, but answering is harmless. */
            unsigned char pong[BB_EIO_OPEN_PACKET_BYTES];
            size_t length = packetLength < sizeof(pong) ? packetLength : sizeof(pong);
            pong[0] = '3';
            for (size_t i = 1; i < length; i++) pong[i] = packet[i];
            return bb_eio_session_send_packet(session, pong, length);
        }
        case BBEIOPacketPong:
            session->awaitingPong = false;
            session->pongsReceived++;
            return true;
        case BBEIOPacketClose:
            bb_eio_session_close(session, BBEIOClosePeerDisconnect);
            return true;
        case BBEIOPacketMessage:
            break;
        default:
            /* Open, upgrade, and noop never come from a client that was
             * offered no upgrades. */
            bb_eio_session_close(session, BBEIOCloseProtocolError);
            return false;
    }

    switch (parsed.socketType) {
        case BBSIOPacketConnect:
            if (!bb_eio_namespace_is_root(packet, &parsed)) {
                static const unsigned char rejection[] =
                    "44{\"message\":\"Invalid namespace\"}";
                return bb_eio_session_send_packet(session, rejection,
                                                  sizeof(rejection) - 1);
            }
            session->state = BBEIOSessionStateConnected;
            return bb_eio_session_send_namespace_connect(session);
        case BBSIOPacketDisconnect:
            bb_eio_session_close(session, BBEIOClosePeerDisconnect);
            return true;
        case BBSIOPacketEvent:
            if (session->state != BBEIOSessionStateConnected) {
                bb_eio_session_close(session, BBEIOCloseProtocolError);
                return false;
            }
            session->eventsReceived++;
            session->callbacks.event(session->callbacks.context, session,
                                     packet, packetLength, &parsed);
            return true;
        case BBSIOPacketAck:
        case BBSIOPacketConnectError:
            /* The server never emits with an ACK id, so nothing to match. */
            return true;
        default:
            bb_eio_session_close(session, BBEIOCloseProtocolError);
            return false;
    }
}

bool bb_eio_session_receive_payload(BBEIOSession *session,
                                    const unsigned char *payload,
                                    size_t payloadLength,
                                    uint64_t nowMs)
{
    size_t cursor = 0;
    size_t offset = 0;
    size_t length = 0;
    if (!bb_eio_session_live(session) || session->transport != BBEIOTransportPolling)
        return false;
    if (!payload || !payloadLength) {
        bb_eio_session_close(session, BBEIOCloseProtocolError);
        return false;
    }
    while (bb_eio_session_live(session) &&
           bb_eio_next_payload_packet(payload, payloadLength, &cursor, &offset, &length)) {
        if (!bb_eio_session_receive_packet(session, payload + offset, length, nowMs))
            return false;
    }
    if (bb_eio_session_live(session) && cursor < payloadLength) {
        /* An empty record means a stray separator. */
        bb_eio_session_close(session, BBEIOCloseProtocolError);
        return false;
    }
    return true;
}

static bool bb_eio_session_send_control_frame(BBEIOSession *session,
                                              unsigned char opcode,
                                              const unsigned char *payload,
                                              size_t payloadLength)
{
    unsigned char frame[2 + 125];
    size_t length = 0;
    if (!bb_ws_build_server_frame(opcode, payload, payloadLength, frame,
                                  sizeof(frame), &length)) return false;
    return session->callbacks.sendFrame(session->callbacks.context, session,
                                        frame, length);
}

static void bb_eio_session_consume_frames(BBEIOSession *session, size_t length)
{
    size_t remaining;
    if (length >= session->framesLength) {
        session->framesLength = 0;
        return;
    }
    remaining = session->framesLength - length;
    bb_eio_memory_copy(session->frames, session->frames + length, remaining);
    session->framesLength = remaining;
}

bool bb_eio_session_receive_bytes(BBEIOSession *session,
                                  const unsigned char *bytes, size_t length,
                                  uint64_t nowMs)
{
    if (!bb_eio_session_live(session) || session->transport != BBEIOTransportWebSocket)
        return false;
    if (length && !bytes) return false;
    if (length > session->framesCapacity - session->framesLength) {
        bb_eio_session_close(session, BBEIOCloseBufferExhausted);
        return false;
    }
    bb_eio_memory_copy(session->frames + session->framesLength, bytes, length);
    session->framesLength += length;

    while (bb_eio_session_live(session) && session->framesLength) {
        BBWSFrame frame;
        BBWSFrameParseResult result = bb_ws_parse_client_frame(session->frames,
                                                               session->framesLength,
                                                               &frame);
        if (result == BBWSFrameNeedMore) {
            if (session->framesLength >= session->framesCapacity) {
                bb_eio_session_close(session, BBEIOCloseBufferExhausted);
                return false;
            }
            break;
        }
        if (result != BBWSFrameReady) {
            bb_eio_session_close(session, BBEIOCloseProtocolError);
            return false;
        }
        switch (frame.opcode) {
            case 0x8:
                (void)bb_eio_session_send_control_frame(session, 0x8, NULL, 0);
                session->closeFrameSent = true;
                bb_eio_session_close(session, BBEIOClosePeerFrame);
                return true;
            case 0x9: {
                unsigned char payload[125];
                size_t payloadLength = 0;
                if (!bb_ws_copy_unmasked_payload(&frame, payload, sizeof(payload),
                                                 &payloadLength) ||
                    !bb_eio_session_send_control_frame(session, 0xA, payload,
                                                       payloadLength)) {
                    bb_eio_session_close(session, BBEIOCloseTransportFailed);
                    return false;
                }
                session->lastActivityAt = nowMs;
                break;
            }
            case 0xA:
                session->lastActivityAt = nowMs;
                break;
            case 0x1:
            case 0x0: {
                size_t copied = 0;
                bool start = frame.opcode == 0x1;
                if (start == session->messageInProgress) {
                    /* New text while fragmented, or continuation with no
                     * message in progress. */
                    bb_eio_session_close(session, BBEIOCloseProtocolError);
                    return false;
                }
                if (start) session->messageLength = 0;
                if (frame.payloadLength > session->messageCapacity - session->messageLength ||
                    !bb_ws_copy_unmasked_payload(&frame,
                                                 session->message + session->messageLength,
                                                 session->messageCapacity - session->messageLength,
                                                 &copied)) {
                    bb_eio_session_close(session, BBEIOCloseBufferExhausted);
                    return false;
                }
                session->messageLength += copied;
                session->messageInProgress = !frame.fin;
                if (frame.fin) {
                    size_t packetLength = session->messageLength;
                    session->messageLength = 0;
                    /* Consume before dispatch: the handler may send, and a
                     * close inside the handler must not re-parse this frame. */
                    bb_eio_session_consume_frames(session, frame.frameLength);
                    if (!bb_eio_session_receive_packet(session, session->message,
                                                       packetLength, nowMs))
                        return false;
                    continue;
                }
                break;
            }
            default:
                /* Binary frames carry Socket.IO binary packets, which the
                 * first profile rejects. */
                bb_eio_session_close(session, BBEIOCloseProtocolError);
                return false;
        }
        bb_eio_session_consume_frames(session, frame.frameLength);
    }
    return true;
}

/* ---- heartbeat, queue, close ----------------------------------------- */

void bb_eio_session_tick(BBEIOSession *session, uint64_t nowMs)
{
    static const unsigned char ping[] = "2";
    if (!bb_eio_session_live(session)) return;
    if (nowMs < session->lastPingSentAt) return;
    if (session->awaitingPong) {
        if (nowMs - session->lastPingSentAt >= BB_EIO_PING_TIMEOUT_MS) {
            bb_eio_session_close(session, BBEIOClosePingTimeout);
        }
        return;
    }
    if (nowMs - session->lastPingSentAt >= BB_EIO_PING_INTERVAL_MS) {
        session->lastPingSentAt = nowMs;
        session->awaitingPong = true;
        session->pingsSent++;
        (void)bb_eio_session_send_packet(session, ping, 1);
    }
}

bool bb_eio_session_drain(BBEIOSession *session, unsigned char *out,
                          size_t capacity, size_t *outLength)
{
    if (!session || session->transport != BBEIOTransportPolling || !out) return false;
    if (!session->outboundLength || session->outboundLength > capacity) return false;
    bb_eio_memory_copy(out, session->outbound, session->outboundLength);
    if (outLength) *outLength = session->outboundLength;
    session->outboundLength = 0;
    return true;
}

size_t bb_eio_session_queued_bytes(const BBEIOSession *session)
{
    if (!session || session->transport != BBEIOTransportPolling) return 0;
    return session->outboundLength;
}

void bb_eio_session_close(BBEIOSession *session, BBEIOCloseReason reason)
{
    if (!bb_eio_session_live(session)) return;
    session->state = BBEIOSessionStateClosed;
    session->messageInProgress = false;
    session->messageLength = 0;
    session->framesLength = 0;
    if (session->transport == BBEIOTransportWebSocket) {
        if (!session->closeFrameSent && reason != BBEIOCloseTransportFailed) {
            (void)bb_eio_session_send_control_frame(session, 0x8, NULL, 0);
            session->closeFrameSent = true;
        }
        session->outboundLength = 0;
    } else {
        /* A held or upcoming polling GET receives the Engine.IO close
         * packet in place of anything still queued. */
        session->outbound[0] = '1';
        session->outboundLength = 1;
    }
    session->callbacks.closed(session->callbacks.context, session, reason);
}

/* ---- table ---------------------------------------------------------- */

BBEIOSession *bb_eio_table_find(const BBEIOSessionTable *table,
                                const char *sid)
{
    if (!table || !table->sessions || !bb_eio_sid_valid(sid)) return NULL;
    for (size_t i = 0; i < table->capacity; i++) {
        BBEIOSession *session = &table->sessions[i];
        if (session->state != BBEIOSessionStateUnused &&
            bb_eio_string_equal(session->sid, sid)) return session;
    }
    return NULL;
}

BBEIOSession *bb_eio_table_take(const BBEIOSessionTable *table,
                                BBEIOTransport transport,
                                const BBEIOCallbacks *callbacks,
                                uint64_t nowMs)
{
    char sid[BB_EIO_SID_BYTES];
    char socketSid[BB_EIO_SID_BYTES];
    if (!table || !table->sessions || !table->generateSid) return NULL;
    for (size_t i = 0; i < table->capacity; i++) {
        BBEIOSession *session = &table->sessions[i];
        if (session->state != BBEIOSessionStateUnused) continue;
        if (!table->generateSid(table->context, sid, sizeof(sid)) ||
            !table->generateSid(table->context, socketSid, sizeof(socketSid)) ||
            bb_eio_string_equal(sid, socketSid) ||
            bb_eio_table_find(table, sid)) return NULL;
        if (!bb_eio_session_init(session, transport, sid, socketSid, callbacks, nowMs))
            return NULL;
        return session;
    }
    return NULL;
}

size_t bb_eio_table_active_count(const BBEIOSessionTable *table)
{
    size_t count = 0;
    if (!table || !table->sessions) return 0;
    for (size_t i = 0; i < table->capacity; i++) {
        if (table->sessions[i].state != BBEIOSessionStateUnused) count++;
    }
    return count;
}

void bb_eio_table_release(BBEIOSession *session)
{
    if (!session || session->state == BBEIOSessionStateUnused) return;
    if (bb_eio_session_live(session)) {
        bb_eio_session_close(session, BBEIOCloseLocal);
    }
    session->state = BBEIOSessionStateUnused;
    session->pollingWaiter = NULL;
    session->outboundLength = 0;
    session->framesLength = 0;
    session->messageLength = 0;
    session->messageInProgress = false;
}
