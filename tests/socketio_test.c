#include "BBSocketIO.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static bool parse_text(const char *text, BBSIOPacket *packet)
{
    return bb_sio_parse_packet((const unsigned char *)text, strlen(text), packet);
}

static void test_engine_control_packets(void)
{
    BBSIOPacket packet;
    CHECK(parse_text("2probe", &packet));
    CHECK(packet.engineType == BBEIOPacketPing && !packet.hasSocketPacket);
    CHECK(parse_text("3probe", &packet));
    CHECK(packet.engineType == BBEIOPacketPong);
    CHECK(parse_text("0{\"sid\":\"fixture\"}", &packet));
    CHECK(packet.engineType == BBEIOPacketOpen);
}

static void test_connect_and_event_ack_id(void)
{
    BBSIOPacket packet;
    CHECK(parse_text("40", &packet));
    CHECK(packet.socketType == BBSIOPacketConnect && !packet.hasAckID);
    CHECK(parse_text("40{\"sid\":\"socket-fixture\"}", &packet));

    const char event[] =
        "42731[\"get-chats\",{\"query\":\"comma,bracket]safe\",\"limit\":100}]";
    CHECK(parse_text(event, &packet));
    CHECK(packet.engineType == BBEIOPacketMessage);
    CHECK(packet.socketType == BBSIOPacketEvent);
    CHECK(packet.hasAckID && packet.ackID == 731);
    CHECK(bb_sio_event_name_equals((const unsigned char *)event, strlen(event),
                                   &packet, "get-chats"));
    CHECK(!bb_sio_event_name_equals((const unsigned char *)event, strlen(event),
                                    &packet, "get-messages"));

    const char escaped[] = "42[\"get\\u002dchats\",{}]";
    CHECK(parse_text(escaped, &packet));
    CHECK(bb_sio_event_name_equals((const unsigned char *)escaped, strlen(escaped),
                                   &packet, "get-chats"));
}

static void test_namespace_and_ack(void)
{
    BBSIOPacket packet;
    const char namespaced[] = "42/bridge,19[\"get-chats\",{}]";
    CHECK(parse_text(namespaced, &packet));
    CHECK(packet.hasNamespace && packet.namespaceLength == 7);
    CHECK(memcmp(namespaced + packet.namespaceOffset, "/bridge", 7) == 0);
    CHECK(packet.hasAckID && packet.ackID == 19);

    CHECK(parse_text("4319[{\"status\":200}]", &packet));
    CHECK(packet.socketType == BBSIOPacketAck && packet.ackID == 19);
    CHECK(parse_text("41", &packet));
    CHECK(packet.socketType == BBSIOPacketDisconnect);
}

static void test_polling_payload(void)
{
    const unsigned char payload[] = "3probe\x1e" "42[\"get-chats\",{}]";
    size_t cursor = 0;
    size_t offset = 0;
    size_t length = 0;
    BBSIOPacket packet;
    CHECK(bb_eio_next_payload_packet(payload, sizeof(payload) - 1, &cursor,
                                     &offset, &length));
    CHECK(length == 6 && bb_sio_parse_packet(payload + offset, length, &packet));
    CHECK(packet.engineType == BBEIOPacketPong);
    CHECK(bb_eio_next_payload_packet(payload, sizeof(payload) - 1, &cursor,
                                     &offset, &length));
    CHECK(bb_sio_parse_packet(payload + offset, length, &packet));
    CHECK(packet.socketType == BBSIOPacketEvent);
    CHECK(!bb_eio_next_payload_packet(payload, sizeof(payload) - 1, &cursor,
                                      &offset, &length));
}

static void test_rejections(void)
{
    BBSIOPacket packet;
    CHECK(!parse_text("", &packet));
    CHECK(!parse_text("7", &packet));
    CHECK(!parse_text("4", &packet));
    CHECK(!parse_text("45-", &packet));
    CHECK(!parse_text("4212", &packet));
    CHECK(!parse_text("42[123,{}]", &packet));
    CHECK(!parse_text("42[\"bad\\q\",{}]", &packet));
    CHECK(!parse_text("43[{\"status\":200}]", &packet));
    CHECK(!parse_text("401{\"sid\":\"x\"}", &packet));
    CHECK(!parse_text("41[]", &packet));

    const unsigned char emptyPacket[] = "2\x1e\x1e" "3";
    size_t cursor = 0;
    size_t offset = 0;
    size_t length = 0;
    CHECK(bb_eio_next_payload_packet(emptyPacket, sizeof(emptyPacket) - 1,
                                     &cursor, &offset, &length));
    CHECK(!bb_eio_next_payload_packet(emptyPacket, sizeof(emptyPacket) - 1,
                                      &cursor, &offset, &length));
}

int main(void)
{
    test_engine_control_packets();
    test_connect_and_event_ack_id();
    test_namespace_and_ack();
    test_polling_payload();
    test_rejections();
    if (failures) return 1;
    puts("socket.io tests passed: engine control, connect, event ACK IDs, polling");
    return 0;
}
