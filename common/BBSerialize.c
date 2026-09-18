#include "BBSerialize.h"

/* FNV-1a over the stable string, folded to 63 bits and forced nonzero. The
 * client only needs originalROWID to be unique and stable per object; the
 * conservative server profile never asks it to order by ROWID. */
int64_t bb_serialize_stable_id(const char *text)
{
    const volatile char *bytes = (const volatile char *)text;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    if (!text) return 1;
    for (size_t i = 0; bytes[i]; i++) {
        hash ^= (unsigned char)bytes[i];
        hash *= UINT64_C(0x100000001b3);
    }
    hash &= UINT64_C(0x7fffffffffffffff);
    if (hash == 0) hash = 1;
    return (int64_t)hash;
}

int64_t bb_serialize_handle_id(const BBHandleRecord *handle)
{
    char identity[512];
    size_t cursor = 0;
    const char *service;
    const char *address;
    if (!handle || !handle->address) return 0;
    service = handle->service ? handle->service : "iMessage";
    address = handle->address;
    for (size_t i = 0; service[i] && cursor + 1 < sizeof(identity); i++) identity[cursor++] = service[i];
    if (cursor + 1 < sizeof(identity)) identity[cursor++] = ';';
    for (size_t i = 0; address[i] && cursor + 1 < sizeof(identity); i++) identity[cursor++] = address[i];
    identity[cursor] = '\0';
    return bb_serialize_stable_id(identity);
}

static bool bb_serialize_key_string(BBJSONWriter *writer, const char *key,
                                    const char *value)
{
    return bb_json_write_key(writer, key) && bb_json_write_string(writer, value);
}

static bool bb_serialize_key_signed(BBJSONWriter *writer, const char *key,
                                    int64_t value)
{
    return bb_json_write_key(writer, key) && bb_json_write_signed(writer, value);
}

static bool bb_serialize_key_optional_ms(BBJSONWriter *writer, const char *key,
                                         int64_t value)
{
    if (!bb_json_write_key(writer, key)) return false;
    return value ? bb_json_write_signed(writer, value) : bb_json_write_null(writer);
}

static bool bb_serialize_key_bool(BBJSONWriter *writer, const char *key, bool value)
{
    return bb_json_write_key(writer, key) && bb_json_write_bool(writer, value);
}

static bool bb_serialize_comma(BBJSONWriter *writer)
{
    return bb_json_write_raw(writer, ",");
}

bool bb_serialize_handle(BBJSONWriter *writer, const BBHandleRecord *handle)
{
    if (!writer || !handle || !handle->address || !handle->address[0]) return false;
    return bb_json_write_raw(writer, "{") &&
           bb_serialize_key_signed(writer, "originalROWID", bb_serialize_handle_id(handle)) &&
           bb_serialize_comma(writer) &&
           bb_serialize_key_string(writer, "address", handle->address) &&
           bb_serialize_comma(writer) &&
           bb_serialize_key_string(writer, "service", handle->service ? handle->service : "iMessage") &&
           bb_serialize_comma(writer) &&
           bb_serialize_key_string(writer, "country", handle->country) &&
           bb_serialize_comma(writer) &&
           bb_serialize_key_string(writer, "uncanonicalizedId", handle->uncanonicalizedID) &&
           bb_json_write_raw(writer, "}");
}

bool bb_serialize_attachment(BBJSONWriter *writer, const BBAttachmentRecord *attachment)
{
    if (!writer || !attachment || !attachment->guid || !attachment->guid[0]) return false;
    if (!bb_json_write_raw(writer, "{") ||
        !bb_serialize_key_signed(writer, "originalROWID", bb_serialize_stable_id(attachment->guid)) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "guid", attachment->guid) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "uti", attachment->uti) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "mimeType", attachment->mimeType) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "transferName", attachment->transferName) ||
        !bb_serialize_comma(writer) ||
        !bb_json_write_key(writer, "totalBytes") ||
        !bb_json_write_unsigned(writer, attachment->totalBytes) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_signed(writer, "transferState", attachment->transferState) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "isOutgoing", attachment->isOutgoing) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "hideAttachment", attachment->hideAttachment) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "isSticker", attachment->isSticker) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "originalGuid", attachment->guid) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "hasLivePhoto", attachment->hasLivePhoto) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_optional_ms(writer, "width", attachment->width) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_optional_ms(writer, "height", attachment->height) ||
        !bb_serialize_comma(writer) ||
        !bb_json_write_key(writer, "metadata") ||
        !bb_json_write_null(writer)) return false;
    return bb_json_write_raw(writer, "}");
}

/* The chat as it appears inside a message's "chats" expansion: identity
 * plus optional participants, never a nested lastMessage. */
static bool bb_serialize_chat_core(BBJSONWriter *writer, const BBChatRecord *chat,
                                   bool withParticipants)
{
    if (!chat || !chat->guid || !chat->guid[0] || !chat->chatIdentifier) return false;
    if (!bb_json_write_raw(writer, "{") ||
        !bb_serialize_key_signed(writer, "originalROWID", bb_serialize_stable_id(chat->guid)) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "guid", chat->guid) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_signed(writer, "style", chat->style) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "chatIdentifier", chat->chatIdentifier) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "isArchived", chat->isArchived) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "displayName", chat->displayName ? chat->displayName : "") ||
        !bb_serialize_comma(writer) ||
        !bb_json_write_raw(writer, "\"groupId\":null,\"properties\":null,\"lastAddressedHandle\":null"))
        return false;
    if (withParticipants) {
        if (!bb_serialize_comma(writer) || !bb_json_write_key(writer, "participants") ||
            !bb_json_write_raw(writer, "[")) return false;
        for (size_t i = 0; i < chat->participantCount; i++) {
            if (i && !bb_serialize_comma(writer)) return false;
            if (!bb_serialize_handle(writer, &chat->participants[i])) return false;
        }
        if (!bb_json_write_raw(writer, "]")) return false;
    }
    return true;
}

bool bb_serialize_message(BBJSONWriter *writer, const BBMessageRecord *message,
                          bool withChats, bool withChatParticipants,
                          bool withAttachments, bool withHandle)
{
    int64_t handleID = 0;
    if (!writer || !message || !message->guid || !message->guid[0] ||
        !message->dateCreated) return false;
    if (message->handle) handleID = bb_serialize_handle_id(message->handle);
    if (!bb_json_write_raw(writer, "{") ||
        !bb_serialize_key_signed(writer, "originalROWID", bb_serialize_stable_id(message->guid)) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "guid", message->guid) ||
        !bb_serialize_comma(writer) ||
        (message->tempGuid && (!bb_serialize_key_string(writer, "tempGuid", message->tempGuid) ||
            !bb_serialize_comma(writer))) ||
        !bb_serialize_key_string(writer, "text", message->text ? message->text : "") ||
        !bb_serialize_comma(writer) ||
        !bb_json_write_key(writer, "handle")) return false;
    if (withHandle && message->handle) {
        if (!bb_serialize_handle(writer, message->handle)) return false;
    } else if (!bb_json_write_null(writer)) {
        return false;
    }
    if (!bb_serialize_comma(writer) ||
        !bb_serialize_key_signed(writer, "handleId", handleID) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_signed(writer, "otherHandle", 0)) return false;
    if (withChats && message->chat) {
        if (!bb_serialize_comma(writer) || !bb_json_write_key(writer, "chats") ||
            !bb_json_write_raw(writer, "[") ||
            !bb_serialize_chat_core(writer, message->chat, withChatParticipants) ||
            !bb_json_write_raw(writer, "}]")) return false;
    }
    if (withAttachments) {
        if (!bb_serialize_comma(writer) || !bb_json_write_key(writer, "attachments") ||
            !bb_json_write_raw(writer, "[")) return false;
        for (size_t i = 0; i < message->attachmentCount; i++) {
            if (i && !bb_serialize_comma(writer)) return false;
            if (!bb_serialize_attachment(writer, &message->attachments[i])) return false;
        }
        if (!bb_json_write_raw(writer, "]")) return false;
    }
    if (!bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "subject", message->subject) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_signed(writer, "error", message->error) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_signed(writer, "dateCreated", message->dateCreated) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_optional_ms(writer, "dateRead", message->dateRead) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_optional_ms(writer, "dateDelivered", message->dateDelivered) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "isDelivered", message->isDelivered) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "isFromMe", message->isFromMe) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "isArchived", false) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_signed(writer, "itemType", message->itemType) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "groupTitle", message->groupTitle) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_signed(writer, "groupActionType", message->groupActionType) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_bool(writer, "hasAttachments", message->attachmentCount != 0) ||
        !bb_serialize_comma(writer) ||
        /* iOS 9 has none of the newer features; say so explicitly. */
        !bb_json_write_raw(writer,
            "\"balloonBundleId\":null,\"associatedMessageGuid\":null,"
            "\"associatedMessageType\":null,\"expressiveSendStyleId\":null,"
            "\"replyToGuid\":null,\"threadOriginatorGuid\":null,"
            "\"dateEdited\":null,\"dateRetracted\":null,\"partCount\":1,"
            "\"messageSummaryInfo\":null,\"payloadData\":null,\"hasPayloadData\":false"))
        return false;
    return bb_json_write_raw(writer, "}");
}

bool bb_serialize_chat(BBJSONWriter *writer, const BBChatRecord *chat,
                       bool withParticipants, bool withLastMessage)
{
    if (!writer || !bb_serialize_chat_core(writer, chat, withParticipants)) return false;
    if (withLastMessage) {
        if (!bb_serialize_comma(writer) || !bb_json_write_key(writer, "lastMessage")) return false;
        if (chat->lastMessage) {
            /* The nested message never re-expands the chat. */
            if (!bb_serialize_message(writer, chat->lastMessage, false, false, true, true))
                return false;
        } else if (!bb_json_write_null(writer)) {
            return false;
        }
    }
    return bb_json_write_raw(writer, "}");
}

bool bb_serialize_chat_with_messages(BBJSONWriter *writer, const BBChatRecord *chat,
                                     const BBMessageRecord *messages, size_t messageCount)
{
    if (!writer || !bb_serialize_chat_core(writer, chat, true)) return false;
    if (!bb_serialize_comma(writer) || !bb_json_write_key(writer, "messages") ||
        !bb_json_write_raw(writer, "[")) return false;
    for (size_t i = 0; i < messageCount; i++) {
        if (i && !bb_serialize_comma(writer)) return false;
        if (!messages || !bb_serialize_message(writer, &messages[i], false, false, true, true))
            return false;
    }
    return bb_json_write_raw(writer, "]") && bb_json_write_raw(writer, "}");
}

static bool bb_serialize_address_list(BBJSONWriter *writer, const char *key,
                                     const BBContactAddress *list, size_t count)
{
    if (!bb_json_write_key(writer, key) || !bb_json_write_raw(writer, "[")) return false;
    for (size_t i = 0; i < count; i++) {
        if (i && !bb_serialize_comma(writer)) return false;
        if (!list[i].address) return false;
        if (!bb_json_write_raw(writer, "{") ||
            !bb_serialize_key_string(writer, "address", list[i].address) ||
            !bb_serialize_comma(writer) ||
            !bb_serialize_key_string(writer, "label", list[i].label ? list[i].label : "") ||
            !bb_json_write_raw(writer, "}")) return false;
    }
    return bb_json_write_raw(writer, "]");
}

bool bb_serialize_contact(BBJSONWriter *writer, const BBContactRecord *contact)
{
    if (!writer || !contact || !contact->identifier || !contact->identifier[0]) return false;
    if (!bb_json_write_raw(writer, "{") ||
        !bb_serialize_key_string(writer, "id", contact->identifier) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "displayName",
                                 contact->displayName ? contact->displayName : "") ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "firstName", contact->firstName) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_key_string(writer, "lastName", contact->lastName) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_address_list(writer, "phoneNumbers", contact->phoneNumbers, contact->phoneCount) ||
        !bb_serialize_comma(writer) ||
        !bb_serialize_address_list(writer, "emails", contact->emails, contact->emailCount) ||
        !bb_serialize_comma(writer) ||
        !bb_json_write_key(writer, "avatar")) return false;
    if (contact->avatarBase64) {
        if (!bb_json_write_string(writer, contact->avatarBase64)) return false;
    } else if (!bb_json_write_null(writer)) {
        return false;
    }
    return bb_json_write_raw(writer, "}");
}

bool bb_serialize_page_metadata(BBJSONWriter *writer, int64_t offset,
                                int64_t limit, int64_t total, int64_t count)
{
    return bb_json_write_raw(writer, "{") &&
           bb_serialize_key_signed(writer, "offset", offset) &&
           bb_serialize_comma(writer) &&
           bb_serialize_key_signed(writer, "limit", limit) &&
           bb_serialize_comma(writer) &&
           bb_serialize_key_signed(writer, "total", total) &&
           bb_serialize_comma(writer) &&
           bb_serialize_key_signed(writer, "count", count) &&
           bb_json_write_raw(writer, "}");
}
