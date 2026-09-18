#ifndef BB_SERIALIZE_H
#define BB_SERIALIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "BBResponse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BlueBubbles response schemas produced from plain C records. The MobileSMS
 * bridge fills these records from IMCore/ChatKit objects on the main queue;
 * this module owns the field lists, defaults, and stable identifiers, and is
 * host-tested. Strings are UTF-8 and may be NULL (serialized as null or as
 * the documented default). Dates are Unix milliseconds; 0 means unknown and
 * is serialized as null where the schema allows null. */

typedef struct {
    const char *address;          /* required */
    const char *service;          /* "iMessage" or "SMS"; NULL → "iMessage" */
    const char *country;          /* nullable */
    const char *uncanonicalizedID;/* nullable */
} BBHandleRecord;

typedef struct {
    const char *guid;             /* required */
    const char *uti;              /* nullable */
    const char *mimeType;         /* nullable */
    const char *transferName;     /* nullable */
    uint64_t totalBytes;
    int transferState;
    bool isOutgoing;
    bool hideAttachment;
    bool isSticker;
    bool hasLivePhoto;
    int64_t width;                /* 0 → null */
    int64_t height;               /* 0 → null */
} BBAttachmentRecord;

typedef struct {
    const char *guid;             /* required */
    const char *tempGuid;         /* echoed on a send response; nullable */
    const char *text;             /* NULL → "" */
    const char *subject;          /* nullable */
    const BBHandleRecord *handle; /* sender; NULL for outgoing or unknown */
    int64_t dateCreated;          /* required, ms */
    int64_t dateRead;             /* 0 → null */
    int64_t dateDelivered;        /* 0 → null */
    bool isFromMe;
    bool isDelivered;
    int error;
    int itemType;                 /* 0 text, 1/3 participant action, 2 name change */
    const char *groupTitle;       /* nullable */
    int groupActionType;
    const BBAttachmentRecord *attachments;
    size_t attachmentCount;
    /* Chat the message belongs to, for "chats":[...] expansions. */
    const struct BBChatRecord *chat;
} BBMessageRecord;

typedef struct BBChatRecord {
    const char *guid;             /* required, "service;-;identifier" form */
    const char *chatIdentifier;   /* required */
    int style;                    /* 43 group, 45 direct */
    bool isArchived;
    const char *displayName;      /* NULL → "" */
    const BBHandleRecord *participants;
    size_t participantCount;
    const BBMessageRecord *lastMessage; /* nullable */
} BBChatRecord;

typedef struct {
    const char *address;          /* phone or email */
    const char *label;            /* "mobile", "home", ...; NULL → "" */
} BBContactAddress;

typedef struct {
    const char *identifier;       /* stable per-contact id (required) */
    const char *displayName;      /* composite name; nullable */
    const char *firstName;        /* nullable */
    const char *lastName;         /* nullable */
    const BBContactAddress *phoneNumbers;
    size_t phoneCount;
    const BBContactAddress *emails;
    size_t emailCount;
    const char *avatarBase64;     /* base64 JPEG or NULL */
} BBContactRecord;

/* Deterministic nonzero 63-bit identifier for a stable string. */
int64_t bb_serialize_stable_id(const char *text);
/* Handle identity is address plus service. */
int64_t bb_serialize_handle_id(const BBHandleRecord *handle);

bool bb_serialize_handle(BBJSONWriter *writer, const BBHandleRecord *handle);
bool bb_serialize_attachment(BBJSONWriter *writer, const BBAttachmentRecord *attachment);
/* withChats adds "chats":[<chat without participants/lastMessage>] when
 * message->chat is set; withAttachments controls the attachments array. */
bool bb_serialize_message(BBJSONWriter *writer, const BBMessageRecord *message,
                          bool withChats, bool withChatParticipants,
                          bool withAttachments, bool withHandle);
bool bb_serialize_chat(BBJSONWriter *writer, const BBChatRecord *chat,
                       bool withParticipants, bool withLastMessage);
/* ChatResponse with participants and "messages":[...], the shape of a
 * POST /chat/new reply (ChatSerializer includeMessages). The nested
 * messages carry no chats expansion; tempGuid is echoed when set. */
bool bb_serialize_chat_with_messages(BBJSONWriter *writer, const BBChatRecord *chat,
                                     const BBMessageRecord *messages, size_t messageCount);
bool bb_serialize_contact(BBJSONWriter *writer, const BBContactRecord *contact);

/* {"offset":o,"limit":l,"total":t,"count":c} */
bool bb_serialize_page_metadata(BBJSONWriter *writer, int64_t offset,
                                int64_t limit, int64_t total, int64_t count);

#ifdef __cplusplus
}
#endif

#endif
