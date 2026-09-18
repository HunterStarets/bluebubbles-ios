#include "BBJSON.h"
#include "BBSerialize.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static char buffer[16384];
static BBJSONWriter writer;

static void begin(void)
{
    bb_json_writer_init(&writer, buffer, sizeof(buffer));
}

static bool finish(void)
{
    BBJSONValue root;
    size_t length = 0;
    if (!bb_json_writer_finish(&writer, &length)) return false;
    /* Everything this module emits must parse. */
    return bb_json_parse(buffer, length, &root);
}

static bool has(const char *needle)
{
    return strstr(buffer, needle) != NULL;
}

/* Fictional records shared by the tests. */
static const BBHandleRecord alice = { "+15555550100", "iMessage", "US", "+1 (555) 555-0100" };
static const BBHandleRecord bob = { "bob@example.invalid", "iMessage", NULL, NULL };
static const BBHandleRecord carol = { "+15555550300", "SMS", NULL, NULL };

static void test_stable_ids(void)
{
    int64_t chat = bb_serialize_stable_id("iMessage;-;+15555550100");
    CHECK(chat > 0);
    CHECK(chat == bb_serialize_stable_id("iMessage;-;+15555550100"));
    CHECK(chat != bb_serialize_stable_id("iMessage;-;+15555550101"));
    CHECK(bb_serialize_stable_id("") > 0);
    CHECK(bb_serialize_stable_id(NULL) == 1);
    CHECK(bb_serialize_handle_id(&alice) > 0);
    CHECK(bb_serialize_handle_id(&alice) == bb_serialize_handle_id(&alice));
    CHECK(bb_serialize_handle_id(&alice) != bb_serialize_handle_id(&bob));
    /* Same address on a different service is a different handle. */
    {
        BBHandleRecord sms = alice;
        sms.service = "SMS";
        CHECK(bb_serialize_handle_id(&sms) != bb_serialize_handle_id(&alice));
    }
    CHECK(bb_serialize_handle_id(NULL) == 0);
}

static void test_handle_and_attachment(void)
{
    BBAttachmentRecord attachment;
    begin();
    CHECK(bb_serialize_handle(&writer, &alice) && finish());
    CHECK(strncmp(buffer, "{\"originalROWID\":", 17) == 0);
    CHECK(has("\"address\":\"+15555550100\",\"service\":\"iMessage\",\"country\":\"US\","
              "\"uncanonicalizedId\":\"+1 (555) 555-0100\"}"));
    begin();
    CHECK(bb_serialize_handle(&writer, &bob) && finish());
    CHECK(has("\"service\":\"iMessage\",\"country\":null,\"uncanonicalizedId\":null}"));
    {
        BBHandleRecord defaulted = { "+15555550999", NULL, NULL, NULL };
        BBHandleRecord empty = { "", NULL, NULL, NULL };
        begin();
        CHECK(bb_serialize_handle(&writer, &defaulted) && finish());
        CHECK(has("\"service\":\"iMessage\""));
        begin();
        CHECK(!bb_serialize_handle(&writer, &empty));
        CHECK(!bb_serialize_handle(&writer, NULL));
    }

    attachment.guid = "at_0_fixture-attachment-1";
    attachment.uti = "public.jpeg";
    attachment.mimeType = "image/jpeg";
    attachment.transferName = "IMG_0001.JPG";
    attachment.totalBytes = 123456;
    attachment.transferState = 5;
    attachment.isOutgoing = false;
    attachment.hideAttachment = false;
    attachment.isSticker = false;
    attachment.hasLivePhoto = false;
    attachment.width = 1024;
    attachment.height = 768;
    begin();
    CHECK(bb_serialize_attachment(&writer, &attachment) && finish());
    CHECK(has("\"guid\":\"at_0_fixture-attachment-1\",\"uti\":\"public.jpeg\",\"mimeType\":\"image/jpeg\","
              "\"transferName\":\"IMG_0001.JPG\",\"totalBytes\":123456,\"transferState\":5,"
              "\"isOutgoing\":false,\"hideAttachment\":false,\"isSticker\":false,"
              "\"originalGuid\":\"at_0_fixture-attachment-1\",\"hasLivePhoto\":false,"
              "\"width\":1024,\"height\":768,\"metadata\":null}"));
    attachment.uti = NULL;
    attachment.mimeType = NULL;
    attachment.transferName = NULL;
    attachment.width = 0;
    attachment.height = 0;
    begin();
    CHECK(bb_serialize_attachment(&writer, &attachment) && finish());
    CHECK(has("\"uti\":null,\"mimeType\":null,\"transferName\":null"));
    CHECK(has("\"width\":null,\"height\":null"));
    attachment.guid = "";
    begin();
    CHECK(!bb_serialize_attachment(&writer, &attachment));
}

static void test_messages(void)
{
    BBHandleRecord participants[2];
    BBChatRecord chat;
    BBMessageRecord incoming;
    BBMessageRecord outgoing;
    BBAttachmentRecord attachment;

    participants[0] = alice;
    participants[1] = bob;
    chat.guid = "iMessage;+;chat123456789";
    chat.chatIdentifier = "chat123456789";
    chat.style = 43;
    chat.isArchived = false;
    chat.displayName = "Fixture Group";
    chat.participants = participants;
    chat.participantCount = 2;
    chat.lastMessage = NULL;

    incoming.guid = "fixture-message-1";
    incoming.tempGuid = NULL;
    incoming.text = "hello \"quoted\" \xc3\xa9";
    incoming.subject = NULL;
    incoming.handle = &alice;
    incoming.dateCreated = INT64_C(1758067200123);
    incoming.dateRead = INT64_C(1758067260000);
    incoming.dateDelivered = 0;
    incoming.isFromMe = false;
    incoming.isDelivered = true;
    incoming.error = 0;
    incoming.itemType = 0;
    incoming.groupTitle = NULL;
    incoming.groupActionType = 0;
    incoming.attachments = NULL;
    incoming.attachmentCount = 0;
    incoming.chat = &chat;

    begin();
    CHECK(bb_serialize_message(&writer, &incoming, true, true, true, true) && finish());
    CHECK(has("\"guid\":\"fixture-message-1\",\"text\":\"hello \\\"quoted\\\" \xc3\xa9\",\"handle\":{"));
    CHECK(has("\"address\":\"+15555550100\""));
    CHECK(has("\"handleId\":"));
    CHECK(has("\"otherHandle\":0,\"chats\":[{"));
    CHECK(has("\"guid\":\"iMessage;+;chat123456789\",\"style\":43,\"chatIdentifier\":\"chat123456789\","
              "\"isArchived\":false,\"displayName\":\"Fixture Group\",\"groupId\":null,\"properties\":null,"
              "\"lastAddressedHandle\":null,\"participants\":[{"));
    CHECK(has("}],\"attachments\":[],\"subject\":null,\"error\":0,\"dateCreated\":1758067200123,"
              "\"dateRead\":1758067260000,\"dateDelivered\":null,\"isDelivered\":true,\"isFromMe\":false,"
              "\"isArchived\":false,\"itemType\":0,\"groupTitle\":null,\"groupActionType\":0,"
              "\"hasAttachments\":false,\"balloonBundleId\":null,\"associatedMessageGuid\":null,"
              "\"associatedMessageType\":null,\"expressiveSendStyleId\":null,\"replyToGuid\":null,"
              "\"threadOriginatorGuid\":null,\"dateEdited\":null,\"dateRetracted\":null,\"partCount\":1,"
              "\"messageSummaryInfo\":null,\"payloadData\":null,\"hasPayloadData\":false}"));
    /* The nested chat never carries a lastMessage. */
    CHECK(!has("lastMessage"));
    /* handleId equals the serialized handle's originalROWID. */
    {
        char expected[64];
        snprintf(expected, sizeof(expected), "\"handleId\":%lld,", (long long)bb_serialize_handle_id(&alice));
        CHECK(has(expected));
        snprintf(expected, sizeof(expected), "\"originalROWID\":%lld,\"address\":\"+15555550100\"",
                 (long long)bb_serialize_handle_id(&alice));
        CHECK(has(expected));
    }

    /* Expansions off: no chats key, no attachments key, handle null. */
    begin();
    CHECK(bb_serialize_message(&writer, &incoming, false, false, false, false) && finish());
    CHECK(!has("\"chats\"") && !has("\"attachments\"") && has("\"handle\":null,\"handleId\":"));
    /* Chats without participants. */
    begin();
    CHECK(bb_serialize_message(&writer, &incoming, true, false, false, true) && finish());
    CHECK(has("\"lastAddressedHandle\":null}]") && !has("participants"));

    attachment.guid = "at_0_fixture-attachment-1";
    attachment.uti = "public.jpeg";
    attachment.mimeType = "image/jpeg";
    attachment.transferName = "IMG_0001.JPG";
    attachment.totalBytes = 10;
    attachment.transferState = 5;
    attachment.isOutgoing = true;
    attachment.hideAttachment = false;
    attachment.isSticker = false;
    attachment.hasLivePhoto = false;
    attachment.width = 0;
    attachment.height = 0;

    outgoing = incoming;
    outgoing.guid = "fixture-message-2";
    outgoing.text = "";
    outgoing.subject = "Subject";
    outgoing.handle = NULL;
    outgoing.dateRead = 0;
    outgoing.dateDelivered = INT64_C(1758067200500);
    outgoing.isFromMe = true;
    outgoing.error = 22;
    outgoing.attachments = &attachment;
    outgoing.attachmentCount = 1;
    outgoing.chat = NULL;
    begin();
    CHECK(bb_serialize_message(&writer, &outgoing, true, true, true, true) && finish());
    CHECK(has("\"text\":\"\",\"handle\":null,\"handleId\":0,\"otherHandle\":0,\"attachments\":[{"));
    CHECK(!has("\"chats\""));
    CHECK(has("\"isOutgoing\":true"));
    CHECK(has("\"subject\":\"Subject\",\"error\":22,\"dateCreated\":1758067200123,\"dateRead\":null,"
              "\"dateDelivered\":1758067200500,\"isDelivered\":true,\"isFromMe\":true"));
    CHECK(has("\"hasAttachments\":true"));

    /* Group actions carry itemType and groupTitle. */
    outgoing.itemType = 2;
    outgoing.groupTitle = "Renamed";
    outgoing.groupActionType = 0;
    begin();
    CHECK(bb_serialize_message(&writer, &outgoing, false, false, false, false) && finish());
    CHECK(has("\"itemType\":2,\"groupTitle\":\"Renamed\",\"groupActionType\":0"));

    /* Send responses echo tempGuid right after guid. */
    outgoing.itemType = 0;
    outgoing.groupTitle = NULL;
    outgoing.tempGuid = "temp-abc-123";
    begin();
    CHECK(bb_serialize_message(&writer, &outgoing, false, false, false, false) && finish());
    CHECK(has("\"guid\":\"fixture-message-2\",\"tempGuid\":\"temp-abc-123\",\"text\":"));
    outgoing.tempGuid = NULL;

    /* Required fields. */
    outgoing.guid = "";
    begin();
    CHECK(!bb_serialize_message(&writer, &outgoing, false, false, false, false));
    outgoing.guid = "x";
    outgoing.dateCreated = 0;
    begin();
    CHECK(!bb_serialize_message(&writer, &outgoing, false, false, false, false));
}

static void test_chats(void)
{
    BBHandleRecord participants[1];
    BBChatRecord direct;
    BBMessageRecord last;

    participants[0] = carol;
    last.guid = "fixture-message-3";
    last.tempGuid = NULL;
    last.text = "last";
    last.subject = NULL;
    last.handle = &carol;
    last.dateCreated = INT64_C(1758067300000);
    last.dateRead = 0;
    last.dateDelivered = 0;
    last.isFromMe = false;
    last.isDelivered = true;
    last.error = 0;
    last.itemType = 0;
    last.groupTitle = NULL;
    last.groupActionType = 0;
    last.attachments = NULL;
    last.attachmentCount = 0;
    last.chat = &direct;

    direct.guid = "SMS;-;+15555550300";
    direct.chatIdentifier = "+15555550300";
    direct.style = 45;
    direct.isArchived = false;
    direct.displayName = NULL;
    direct.participants = participants;
    direct.participantCount = 1;
    direct.lastMessage = &last;

    begin();
    CHECK(bb_serialize_chat(&writer, &direct, true, true) && finish());
    CHECK(has("{\"originalROWID\":"));
    CHECK(has("\"guid\":\"SMS;-;+15555550300\",\"style\":45,\"chatIdentifier\":\"+15555550300\","
              "\"isArchived\":false,\"displayName\":\"\",\"groupId\":null,\"properties\":null,"
              "\"lastAddressedHandle\":null,\"participants\":[{"));
    CHECK(has("\"address\":\"+15555550300\",\"service\":\"SMS\""));
    CHECK(has("}],\"lastMessage\":{\"originalROWID\":"));
    CHECK(has("\"guid\":\"fixture-message-3\",\"text\":\"last\",\"handle\":{"));
    /* The last message inside a chat does not expand chats again. */
    CHECK(strstr(buffer, "\"chats\"") == NULL);
    CHECK(has("\"attachments\":[]"));

    begin();
    CHECK(bb_serialize_chat(&writer, &direct, false, false) && finish());
    CHECK(!has("participants") && !has("lastMessage"));
    begin();
    direct.lastMessage = NULL;
    CHECK(bb_serialize_chat(&writer, &direct, true, true) && finish());
    CHECK(has("\"lastMessage\":null}"));

    direct.guid = NULL;
    begin();
    CHECK(!bb_serialize_chat(&writer, &direct, true, true));
    direct.guid = "SMS;-;+15555550300";
    direct.chatIdentifier = NULL;
    begin();
    CHECK(!bb_serialize_chat(&writer, &direct, true, true));

    begin();
    CHECK(bb_serialize_page_metadata(&writer, 0, 25, 3, 3) && finish());
    CHECK(strcmp(buffer, "{\"offset\":0,\"limit\":25,\"total\":3,\"count\":3}") == 0);

    /* POST /chat/new shape: participants plus the first sent message,
     * tempGuid echoed, no chats expansion inside the message. */
    direct.chatIdentifier = "+15555550300";
    direct.lastMessage = NULL;
    last.tempGuid = "temp-7";
    last.isFromMe = true;
    last.handle = NULL;
    begin();
    CHECK(bb_serialize_chat_with_messages(&writer, &direct, &last, 1) && finish());
    CHECK(has("\"lastAddressedHandle\":null,\"participants\":[{"));
    CHECK(has("}],\"messages\":[{\"originalROWID\":"));
    CHECK(has("\"guid\":\"fixture-message-3\",\"tempGuid\":\"temp-7\",\"text\":\"last\",\"handle\":null"));
    CHECK(has("\"isFromMe\":true"));
    CHECK(strstr(buffer, "\"chats\"") == NULL);
    CHECK(strstr(buffer, "\"lastMessage\"") == NULL);
    CHECK(buffer[strlen(buffer) - 2] == ']' && buffer[strlen(buffer) - 1] == '}');
    begin();
    CHECK(bb_serialize_chat_with_messages(&writer, &direct, NULL, 0) && finish());
    CHECK(has("\"messages\":[]}"));
    begin();
    CHECK(!bb_serialize_chat_with_messages(&writer, &direct, NULL, 1));
    last.tempGuid = NULL;
    last.isFromMe = false;
    last.handle = &carol;

    /* Overflow is reported, never truncated silently. */
    {
        char small[64];
        BBJSONWriter tiny;
        bb_json_writer_init(&tiny, small, sizeof(small));
        CHECK(!bb_serialize_chat(&tiny, &direct, true, true));
        CHECK(!bb_json_writer_finish(&tiny, NULL));
    }
}

static void test_contacts(void)
{
    BBContactAddress phones[2];
    BBContactAddress emails[1];
    BBContactRecord contact;
    phones[0].address = "+15555550100"; phones[0].label = "mobile";
    phones[1].address = "+15555550111"; phones[1].label = NULL;
    emails[0].address = "member@example.invalid"; emails[0].label = "home";
    contact.identifier = "ab-42";
    contact.displayName = "Fixture Person";
    contact.firstName = "Fixture";
    contact.lastName = "Person";
    contact.phoneNumbers = phones; contact.phoneCount = 2;
    contact.emails = emails; contact.emailCount = 1;
    contact.avatarBase64 = NULL;

    begin();
    CHECK(bb_serialize_contact(&writer, &contact) && finish());
    CHECK(has("{\"id\":\"ab-42\",\"displayName\":\"Fixture Person\",\"firstName\":\"Fixture\","
              "\"lastName\":\"Person\",\"phoneNumbers\":[{\"address\":\"+15555550100\",\"label\":\"mobile\"},"
              "{\"address\":\"+15555550111\",\"label\":\"\"}],\"emails\":[{\"address\":\"member@example.invalid\","
              "\"label\":\"home\"}],\"avatar\":null}"));
    /* Avatar present, no phones/emails, no names. */
    contact.displayName = NULL; contact.firstName = NULL; contact.lastName = NULL;
    contact.phoneCount = 0; contact.emailCount = 0;
    contact.avatarBase64 = "AAAA";
    begin();
    CHECK(bb_serialize_contact(&writer, &contact) && finish());
    CHECK(has("\"displayName\":\"\",\"firstName\":null,\"lastName\":null,"
              "\"phoneNumbers\":[],\"emails\":[],\"avatar\":\"AAAA\"}"));
    contact.identifier = "";
    begin();
    CHECK(!bb_serialize_contact(&writer, &contact));
}

int main(void)
{
    test_stable_ids();
    test_contacts();
    test_handle_and_attachment();
    test_messages();
    test_chats();
    if (failures) return 1;
    puts("serialize tests passed: stable ids, handles, attachments, messages, chats, metadata");
    return 0;
}
