#import "BBBridgeRequests.h"

#import <Foundation/NSDistributedNotificationCenter.h>
#import <AppSupport/CPDistributedMessagingCenter.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <AddressBook/AddressBook.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#import "BBBridgeRuntime.h"
#import "BBTrace.h"

#include "BBJSON.h"
#include "BBResponse.h"
#include "BBSerialize.h"

/* Every private-API call below is dynamically checked: the class must exist,
 * the receiver must respond, and the method signature's return/argument
 * types must match before objc_msgSend is cast. Missing selectors produce
 * truthful defaults (null/false/0), never invented values. All IMCore and
 * ChatKit access happens on the main queue inside @try, as in the rest of the
 * bridge. Diagnostics stay count-only. */

static NSString * const BBBridgeRequestNotificationName = @"com.bluebubbles.bridge.request";
static NSString * const BBBridgeReplyMessageName = @"reply";
static NSString * const BBBridgeEventMessageName = @"event";

#define BB_BRIDGE_REPLY_BYTES (4U * 1024U * 1024U)
#define BB_BRIDGE_MAX_PARTICIPANTS 64U
#define BB_BRIDGE_MAX_ATTACHMENTS 16U
#define BB_BRIDGE_MAX_LOAD 1000U
#define BB_BRIDGE_LOAD_DELAY_NS 100000000LL
#define BB_BRIDGE_MAX_GUID_BYTES 256U

/* ---- dynamic call helpers ---------------------------------------------- */

static const char *BBRQUnqualifiedType(const char *type)
{
    if (!type) return NULL;
    while (*type == 'r' || *type == 'n' || *type == 'N' ||
           *type == 'o' || *type == 'O' || *type == 'R' || *type == 'V') type++;
    return type;
}

static NSMethodSignature *BBRQSignature(id receiver, SEL selector, NSUInteger arguments)
{
    if (!receiver || !selector || ![receiver respondsToSelector:selector]) return nil;
    NSMethodSignature *signature = [receiver methodSignatureForSelector:selector];
    if (!signature || [signature numberOfArguments] != arguments) return nil;
    return signature;
}

static id BBRQObject(id receiver, const char *name)
{
    SEL selector = sel_registerName(name);
    NSMethodSignature *signature = BBRQSignature(receiver, selector, 2);
    const char *type;
    if (!signature) return nil;
    type = BBRQUnqualifiedType([signature methodReturnType]);
    if (!type || *type != '@') return nil;
    return ((id (*)(id, SEL))objc_msgSend)(receiver, selector);
}

static id BBRQObjectWithArgument(id receiver, const char *name, id argument)
{
    SEL selector = sel_registerName(name);
    NSMethodSignature *signature = BBRQSignature(receiver, selector, 3);
    const char *type;
    const char *argumentType;
    if (!signature) return nil;
    type = BBRQUnqualifiedType([signature methodReturnType]);
    argumentType = BBRQUnqualifiedType([signature getArgumentTypeAtIndex:2]);
    if (!type || *type != '@' || !argumentType || *argumentType != '@') return nil;
    return ((id (*)(id, SEL, id))objc_msgSend)(receiver, selector, argument);
}

static id BBRQObjectWithThreeArguments(id receiver, const char *name, id first, id second, id third)
{
    SEL selector = sel_registerName(name);
    NSMethodSignature *signature = BBRQSignature(receiver, selector, 5);
    const char *type;
    if (!signature) return nil;
    type = BBRQUnqualifiedType([signature methodReturnType]);
    if (!type || *type != '@') return nil;
    for (NSUInteger index = 2; index < 5; index++) {
        const char *argumentType = BBRQUnqualifiedType([signature getArgumentTypeAtIndex:index]);
        if (!argumentType || *argumentType != '@') return nil;
    }
    return ((id (*)(id, SEL, id, id, id))objc_msgSend)(receiver, selector, first, second, third);
}

static BOOL BBRQBool(id receiver, const char *name, BOOL *present)
{
    SEL selector = sel_registerName(name);
    NSMethodSignature *signature = BBRQSignature(receiver, selector, 2);
    const char *type;
    if (present) *present = NO;
    if (!signature) return NO;
    type = BBRQUnqualifiedType([signature methodReturnType]);
    if (!type) return NO;
    switch (*type) {
        case 'B': if (present) *present = YES; return ((BOOL (*)(id, SEL))objc_msgSend)(receiver, selector);
        case 'c': if (present) *present = YES; return ((signed char (*)(id, SEL))objc_msgSend)(receiver, selector) != 0;
        case 'C': if (present) *present = YES; return ((unsigned char (*)(id, SEL))objc_msgSend)(receiver, selector) != 0;
        default: return NO;
    }
}

static long long BBRQInteger(id receiver, const char *name)
{
    SEL selector = sel_registerName(name);
    NSMethodSignature *signature = BBRQSignature(receiver, selector, 2);
    const char *type;
    if (!signature) return 0;
    type = BBRQUnqualifiedType([signature methodReturnType]);
    if (!type) return 0;
    switch (*type) {
        case 'i': return ((int (*)(id, SEL))objc_msgSend)(receiver, selector);
        case 'I': return ((unsigned int (*)(id, SEL))objc_msgSend)(receiver, selector);
        case 'l': return ((long (*)(id, SEL))objc_msgSend)(receiver, selector);
        case 'L': return ((unsigned long (*)(id, SEL))objc_msgSend)(receiver, selector);
        case 'q': return ((long long (*)(id, SEL))objc_msgSend)(receiver, selector);
        case 'Q': {
            unsigned long long value = ((unsigned long long (*)(id, SEL))objc_msgSend)(receiver, selector);
            return value > (unsigned long long)LLONG_MAX ? LLONG_MAX : (long long)value;
        }
        case 'c': return ((signed char (*)(id, SEL))objc_msgSend)(receiver, selector);
        case 's': return ((short (*)(id, SEL))objc_msgSend)(receiver, selector);
        default: return 0;
    }
}

static NSString *BBRQString(id receiver, const char *name)
{
    id value = BBRQObject(receiver, name);
    if ([value isKindOfClass:[NSString class]]) return value;
    if ([value isKindOfClass:[NSAttributedString class]]) return [(NSAttributedString *)value string];
    if ([value isKindOfClass:[NSNumber class]]) return [value stringValue];
    return nil;
}

static NSArray *BBRQArray(id receiver, const char *name)
{
    id value = BBRQObject(receiver, name);
    if ([value isKindOfClass:[NSArray class]]) return [[value copy] autorelease];
    if ([value isKindOfClass:[NSSet class]]) return [(NSSet *)value allObjects];
    if ([value isKindOfClass:[NSOrderedSet class]]) return [(NSOrderedSet *)value array];
    return nil;
}

static int64_t BBRQMillis(id value)
{
    if (![value isKindOfClass:[NSDate class]]) return 0;
    NSTimeInterval seconds = [(NSDate *)value timeIntervalSince1970];
    if (seconds <= 0) return 0;
    return (int64_t)(seconds * 1000.0);
}

static int64_t BBRQDateMillis(id receiver, const char *name)
{
    return BBRQMillis(BBRQObject(receiver, name));
}

/* UTF-8 bytes of a string that stay valid while the enclosing autorelease
 * pool lives (the request is served inside one). */
static const char *BBRQUTF8(NSString *value)
{
    if (![value isKindOfClass:[NSString class]] || ![value length]) return NULL;
    return [value UTF8String];
}

/* ---- conversations ------------------------------------------------------ */

static NSArray *BBRQConversations(void)
{
    Class listClass = NSClassFromString(@"CKConversationList");
    id sharedList;
    if (!listClass ||
        !class_respondsToSelector(object_getClass(listClass), @selector(sharedConversationList)))
        return nil;
    sharedList = BBRQObject((id)listClass, "sharedConversationList");
    return BBRQArray(sharedList, "conversations");
}

static id BBRQChat(id conversation)
{
    return BBRQObject(conversation, "chat");
}

static NSString *BBRQServiceName(id owner)
{
    /* IMHandle/IMChat -service → IMServiceImpl -name; IMChat -account -service. */
    id service = BBRQObject(owner, "service");
    NSString *name = BBRQString(service, "name");
    if ([name length]) return name;
    name = BBRQString(service, "internalName");
    if ([name length]) return name;
    {
        id account = BBRQObject(owner, "account");
        NSString *accountService = BBRQString(account, "serviceName");
        if ([accountService length]) return accountService;
        service = BBRQObject(account, "service");
        name = BBRQString(service, "name");
        if ([name length]) return name;
    }
    return nil;
}

static NSString *BBRQNormalizedService(NSString *name)
{
    if (![name length]) return @"iMessage";
    if ([name caseInsensitiveCompare:@"SMS"] == NSOrderedSame) return @"SMS";
    if ([name rangeOfString:@"sms" options:NSCaseInsensitiveSearch].location != NSNotFound) return @"SMS";
    return @"iMessage";
}

static NSString *BBRQConversationIdentifier(id conversation)
{
    NSString *value = BBRQString(conversation, "persistentID");
    if ([value length]) return value;
    value = BBRQString(conversation, "groupID");
    if ([value length]) return value;
    return BBRQString(conversation, "uniqueIdentifier");
}

static BOOL BBRQIsGroup(id conversation, NSArray *handles)
{
    BOOL present = NO;
    BOOL group = BBRQBool(conversation, "isGroupConversation", &present);
    if (present) return group;
    return [handles count] > 1;
}

/* Fill a handle record from an IMHandle-like object. Strings are kept alive
 * by the current autorelease pool. */
static BOOL BBRQFillHandle(id handle, NSString *fallbackService, BBHandleRecord *record)
{
    NSString *address = BBRQString(handle, "ID");
    NSString *service;
    if (![address length]) address = BBRQString(handle, "unformattedID");
    if (![address length]) return NO;
    service = BBRQServiceName(handle);
    record->address = BBRQUTF8(address);
    record->service = BBRQUTF8(BBRQNormalizedService([service length] ? service : fallbackService));
    record->country = BBRQUTF8(BBRQString(handle, "countryCode"));
    record->uncanonicalizedID = BBRQUTF8(BBRQString(handle, "formattedID"));
    return record->address != NULL;
}

static NSString *BBRQChatGuid(id conversation, id chat, NSString *identifier,
                              BOOL group, NSString *service)
{
    NSString *guid = BBRQString(chat, "guid");
    if ([guid length] && [guid rangeOfString:@";"].location != NSNotFound) return guid;
    (void)conversation;
    return [NSString stringWithFormat:@"%@;%@;%@", BBRQNormalizedService(service),
            group ? @"+" : @"-", identifier];
}

/* Fill a chat record and its participants. participants must hold
 * BB_BRIDGE_MAX_PARTICIPANTS records. */
static BOOL BBRQFillChat(id conversation, BBChatRecord *record,
                         BBHandleRecord *participants, size_t *participantCount)
{
    id chat = BBRQChat(conversation);
    NSArray *handles = BBRQArray(conversation, "handles");
    NSString *service = BBRQServiceName(chat);
    NSString *identifier = BBRQString(chat, "chatIdentifier");
    NSString *fallbackIdentifier = BBRQConversationIdentifier(conversation);
    BOOL group;
    size_t count = 0;

    if (![handles count]) handles = BBRQArray(chat, "participants");
    group = BBRQIsGroup(conversation, handles);
    if (![identifier length]) identifier = fallbackIdentifier;
    if (![identifier length]) return NO;
    if (![service length] && [handles count]) service = BBRQServiceName([handles objectAtIndex:0]);

    for (id handle in handles) {
        if (count >= BB_BRIDGE_MAX_PARTICIPANTS) break;
        if (BBRQFillHandle(handle, service, &participants[count])) count++;
    }

    record->guid = BBRQUTF8(BBRQChatGuid(conversation, chat, identifier, group, service));
    record->chatIdentifier = BBRQUTF8(identifier);
    record->style = group ? 43 : 45;
    record->isArchived = NO;
    /* Direct chats leave the name to the client's contact lookup. */
    record->displayName = group ? BBRQUTF8(BBRQString(conversation, "name")) : "";
    if (group && !record->displayName) record->displayName = BBRQUTF8(BBRQString(chat, "displayName"));
    record->participants = participants;
    record->participantCount = count;
    record->lastMessage = NULL;
    *participantCount = count;
    return record->guid != NULL && record->chatIdentifier != NULL;
}

/* Does this conversation answer to the given guid or alias? */
static BOOL BBRQConversationMatches(id conversation, NSString *wanted)
{
    id chat = BBRQChat(conversation);
    NSString *candidate;
    if (![wanted length]) return NO;
    candidate = BBRQString(chat, "guid");
    if ([candidate isEqualToString:wanted]) return YES;
    candidate = BBRQString(chat, "chatIdentifier");
    if ([candidate isEqualToString:wanted]) return YES;
    candidate = BBRQConversationIdentifier(conversation);
    if ([candidate isEqualToString:wanted]) return YES;
    candidate = BBRQString(conversation, "persistentID");
    if ([candidate isEqualToString:wanted]) return YES;
    candidate = BBRQString(conversation, "groupID");
    if ([candidate isEqualToString:wanted]) return YES;
    candidate = BBRQString(conversation, "uniqueIdentifier");
    if ([candidate isEqualToString:wanted]) return YES;
    /* A composed guid for a chat whose IMChat has no guid selector. */
    {
        BBChatRecord record;
        BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
        size_t count = 0;
        if (BBRQFillChat(conversation, &record, participants, &count) && record.guid &&
            [wanted isEqualToString:[NSString stringWithUTF8String:record.guid]]) return YES;
    }
    return NO;
}

/* The ChatKit conversation for an IMChat identifier, whether or not the
 * conversation list enumerates it yet. Device-proven on iOS 9.3.5: a chat
 * created by IMChatRegistry resolves here immediately, while
 * CKConversationList.conversations lists it only once it holds a message.
 * "Existing" in the selector name is literal: nothing is created. */
static id BBRQConversationForIdentifier(NSString *identifier)
{
    Class listClass = NSClassFromString(@"CKConversationList");
    id sharedList;
    if (![identifier length] || !listClass ||
        !class_respondsToSelector(object_getClass(listClass), @selector(sharedConversationList)))
        return nil;
    sharedList = BBRQObject((id)listClass, "sharedConversationList");
    return BBRQObjectWithArgument(sharedList, "conversationForExistingChatWithGroupID:", identifier);
}

/* "iMessage;-;+15555550100" → "+15555550100"; other strings pass through. */
static NSString *BBRQIdentifierFromGuid(NSString *guid)
{
    NSRange marker;
    if (![guid length]) return nil;
    marker = [guid rangeOfString:@";-;"];
    if (marker.location == NSNotFound) marker = [guid rangeOfString:@";+;"];
    if (marker.location == NSNotFound) return guid;
    return [guid substringFromIndex:marker.location + marker.length];
}

static id BBRQFindConversation(NSArray *conversations, NSString *wanted)
{
    for (id conversation in conversations) {
        if (BBRQConversationMatches(conversation, wanted)) return conversation;
    }
    /* Not enumerated (yet): a chat created moments ago, or one whose only
     * message is still in flight. Resolve it by identifier instead. */
    return BBRQConversationForIdentifier(BBRQIdentifierFromGuid(wanted));
}

/* ---- attachments and messages ------------------------------------------ */

static id BBRQTransferForGUID(NSString *guid)
{
    Class cls = NSClassFromString(@"IMFileTransferCenter");
    id center;
    if (!cls || ![guid length]) return nil;
    center = BBRQObject((id)cls, "sharedInstance");
    return BBRQObjectWithArgument(center, "transferForGUID:", guid);
}

static BOOL BBRQFillAttachment(NSString *guid, id transfer, BBAttachmentRecord *record)
{
    NSString *name;
    BOOL present = NO;
    BOOL incoming;
    if (![guid length]) return NO;
    record->guid = BBRQUTF8(guid);
    record->uti = BBRQUTF8(BBRQString(transfer, "type"));
    record->mimeType = BBRQUTF8(BBRQString(transfer, "mimeType"));
    name = BBRQString(transfer, "filename");
    if (![name length]) {
        id url = BBRQObject(transfer, "localURL");
        if ([url isKindOfClass:[NSURL class]]) name = [(NSURL *)url lastPathComponent];
    }
    if (![name length]) {
        NSString *path = BBRQString(transfer, "localPath");
        if ([path length]) name = [path lastPathComponent];
    }
    record->transferName = BBRQUTF8(name);
    record->totalBytes = (uint64_t)BBRQInteger(transfer, "totalBytes");
    record->transferState = (int)BBRQInteger(transfer, "transferState");
    incoming = BBRQBool(transfer, "isIncoming", &present);
    record->isOutgoing = present ? !incoming : NO;
    record->hideAttachment = NO;
    record->isSticker = NO;
    record->hasLivePhoto = NO;
    record->width = 0;
    record->height = 0;
    return record->guid != NULL;
}

/* The sender handle object of a chat item, resolved the proven way. */
static id BBRQSenderHandle(id item, id message)
{
    static const char *const selectors[] = { "sender", "senderHandle", "handle", "entity", "participant" };
    for (size_t i = 0; i < sizeof(selectors) / sizeof(selectors[0]); i++) {
        id value = BBRQObject(item, selectors[i]);
        if (value && ![value isKindOfClass:[NSString class]]) return value;
    }
    {
        id value = BBRQObject(message, "sender");
        if (value && ![value isKindOfClass:[NSString class]]) return value;
    }
    return nil;
}

/* Fill a message record from a chat item (nullable) and its IMMessage. */
static BOOL BBRQFillMessage(id item, id message, const BBChatRecord *chat,
                            NSString *chatService, BBMessageRecord *record,
                            BBHandleRecord *sender, BBAttachmentRecord *attachments,
                            size_t *attachmentCount, BOOL withAttachments)
{
    NSString *guid = BBRQString(message, "guid");
    NSString *text;
    int64_t created;
    BOOL present = NO;
    size_t count = 0;

    if (![guid length]) guid = BBRQString(item, "guid");
    if (![guid length]) return NO;
    created = BBRQDateMillis(item, "time");
    if (!created) created = BBRQDateMillis(message, "time");
    if (!created) return NO;

    text = BBRQString(item, "text");
    if (!text) text = BBRQString(message, "text");
    if (!text) text = BBRQString(message, "plainBody");

    record->guid = BBRQUTF8(guid);
    record->tempGuid = NULL;
    record->text = BBRQUTF8(text);
    record->subject = BBRQUTF8(BBRQString(message, "subject"));
    record->dateCreated = created;
    record->dateRead = BBRQDateMillis(message, "timeRead");
    record->dateDelivered = BBRQDateMillis(message, "timeDelivered");
    record->isFromMe = BBRQBool(message, "isFromMe", &present);
    record->isDelivered = BBRQBool(message, "isDelivered", &present);
    if (!present && record->dateDelivered) record->isDelivered = YES;
    record->error = (int)BBRQInteger(message, "errorCode");
    if (!record->error) {
        id error = BBRQObject(message, "error");
        if ([error isKindOfClass:[NSError class]]) record->error = (int)[(NSError *)error code];
    }
    record->itemType = 0;
    record->groupTitle = NULL;
    record->groupActionType = 0;
    record->handle = NULL;
    if (!record->isFromMe) {
        id handle = BBRQSenderHandle(item, message);
        if (handle && BBRQFillHandle(handle, chatService, sender)) record->handle = sender;
    }

    if (withAttachments) {
        NSMutableArray *guids = [NSMutableArray array];
        NSString *single = BBRQString(item, "transferGUID");
        NSArray *list;
        if (![single length]) single = BBRQString(message, "transferGUID");
        if ([single length]) [guids addObject:single];
        list = BBRQArray(message, "fileTransferGUIDs");
        for (id value in list) {
            if ([value isKindOfClass:[NSString class]] && [value length] &&
                ![guids containsObject:value]) [guids addObject:value];
        }
        for (NSString *transferGuid in guids) {
            if (count >= BB_BRIDGE_MAX_ATTACHMENTS) break;
            if (BBRQFillAttachment(transferGuid, BBRQTransferForGUID(transferGuid),
                                   &attachments[count])) count++;
        }
    }
    record->attachments = attachments;
    record->attachmentCount = count;
    record->chat = chat;
    *attachmentCount = count;
    return YES;
}

static BOOL BBRQItemIsMessage(id item)
{
    static Class textClass;
    static Class attachmentClass;
    static Class audioClass;
    static Class partClass;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        textClass = NSClassFromString(@"IMTextMessagePartChatItem");
        attachmentClass = NSClassFromString(@"IMAttachmentMessagePartChatItem");
        audioClass = NSClassFromString(@"IMAudioMessageChatItem");
        partClass = NSClassFromString(@"IMMessagePartChatItem");
    });
    if (textClass && [item isKindOfClass:textClass]) return YES;
    if (attachmentClass && [item isKindOfClass:attachmentClass]) return YES;
    if (audioClass && [item isKindOfClass:audioClass]) return YES;
    if (partClass && [item isKindOfClass:partClass]) return YES;
    return NO;
}

static BOOL BBRQLoadMessages(id chat, NSUInteger limit)
{
    SEL selector = sel_registerName("loadMessagesBeforeDate:limit:");
    NSMethodSignature *signature = BBRQSignature(chat, selector, 4);
    const char *dateType;
    const char *limitType;
    if (!signature) return NO;
    dateType = BBRQUnqualifiedType([signature getArgumentTypeAtIndex:2]);
    limitType = BBRQUnqualifiedType([signature getArgumentTypeAtIndex:3]);
    if (!dateType || *dateType != '@' || !limitType ||
        (*limitType != 'I' && *limitType != 'L' && *limitType != 'Q' &&
         *limitType != 'i' && *limitType != 'l' && *limitType != 'q')) return NO;
    ((void (*)(id, SEL, id, NSUInteger))objc_msgSend)(chat, selector, nil, limit);
    return YES;
}

/* ---- arguments ---------------------------------------------------------- */

typedef struct {
    NSString *guid;
    int64_t offset;
    int64_t limit;
    BOOL hasAfter;
    int64_t after;
    BOOL hasBefore;
    int64_t before;
    BOOL sortAscending;
    BOOL sortByLastMessage;
    BOOL withLastMessage;
    BOOL withArchived;
    BOOL withAttachments;
    BOOL withHandle;
    BOOL withChats;
    BOOL withChatParticipants;
    NSString *message;   /* send / chat.new */
    NSString *tempGuid;  /* send / chat.new (optional there) */
    NSArray *addresses;  /* chat.new: NSString handles */
    NSString *service;   /* chat.new: "iMessage" or "SMS" */
    NSString *name;      /* message.attachment: file name shown to the recipient */
    NSString *uploadPath;/* message.attachment: staged file from SpringBoard */
} BBRQArguments;

static void BBRQReadBool(const char *json, const BBJSONValue *root, const char *key, BOOL *out)
{
    BBJSONValue value;
    bool flag = false;
    if (bb_json_object_get(json, root, key, &value) && bb_json_bool(json, &value, &flag)) *out = flag;
}

static BOOL BBRQReadInteger(const char *json, const BBJSONValue *root, const char *key, int64_t *out)
{
    BBJSONValue value;
    if (!bb_json_object_get(json, root, key, &value) || value.type == BBJSONTypeNull) return NO;
    return bb_json_int64(json, &value, out) ? YES : NO;
}

static BOOL BBRQParseArguments(NSString *argumentsJSON, BBRQArguments *arguments)
{
    const char *json = [argumentsJSON UTF8String];
    size_t length = json ? (size_t)[argumentsJSON lengthOfBytesUsingEncoding:NSUTF8StringEncoding] : 0;
    BBJSONValue root;
    BBJSONValue value;
    char text[BB_BRIDGE_MAX_GUID_BYTES];

    arguments->guid = nil;
    arguments->offset = 0;
    arguments->limit = 100;
    arguments->hasAfter = NO;
    arguments->after = 0;
    arguments->hasBefore = NO;
    arguments->before = 0;
    arguments->sortAscending = NO;
    arguments->sortByLastMessage = NO;
    arguments->withLastMessage = NO;
    arguments->withArchived = NO;
    arguments->withAttachments = NO;
    arguments->withHandle = YES;
    arguments->withChats = NO;
    arguments->withChatParticipants = NO;
    arguments->message = nil;
    arguments->tempGuid = nil;
    arguments->addresses = nil;
    arguments->service = nil;
    arguments->name = nil;
    arguments->uploadPath = nil;
    if (!json || !length || !bb_json_parse(json, length, &root) || root.type != BBJSONTypeObject)
        return NO;

    if ((bb_json_object_get(json, &root, "guid", &value) ||
         bb_json_object_get(json, &root, "chatGuid", &value)) &&
        value.type == BBJSONTypeString &&
        bb_json_string_copy(json, &value, text, sizeof(text), NULL) && text[0]) {
        arguments->guid = [NSString stringWithUTF8String:text];
    }
    if (bb_json_object_get(json, &root, "message", &value) && value.type == BBJSONTypeString) {
        char text[4096];
        if (bb_json_string_copy(json, &value, text, sizeof(text), NULL))
            arguments->message = [NSString stringWithUTF8String:text];
    }
    if (bb_json_object_get(json, &root, "tempGuid", &value) && value.type == BBJSONTypeString) {
        char text[128];
        if (bb_json_string_copy(json, &value, text, sizeof(text), NULL))
            arguments->tempGuid = [NSString stringWithUTF8String:text];
    }
    if (bb_json_object_get(json, &root, "addresses", &value) && value.type == BBJSONTypeArray) {
        NSMutableArray *addresses = [NSMutableArray arrayWithCapacity:8];
        BBJSONValue element;
        size_t cursor = 0;
        while (bb_json_array_next(json, &value, &cursor, &element)) {
            char address[256];
            if (element.type != BBJSONTypeString ||
                !bb_json_string_copy(json, &element, address, sizeof(address), NULL) || !address[0])
                return NO;
            [addresses addObject:[NSString stringWithUTF8String:address]];
        }
        arguments->addresses = addresses;
    }
    if (bb_json_object_get(json, &root, "name", &value) && value.type == BBJSONTypeString) {
        char text[256];
        if (bb_json_string_copy(json, &value, text, sizeof(text), NULL))
            arguments->name = [NSString stringWithUTF8String:text];
    }
    if (bb_json_object_get(json, &root, "uploadPath", &value) && value.type == BBJSONTypeString) {
        char text[256];
        if (bb_json_string_copy(json, &value, text, sizeof(text), NULL))
            arguments->uploadPath = [NSString stringWithUTF8String:text];
    }
    if (bb_json_object_get(json, &root, "service", &value) && value.type == BBJSONTypeString) {
        arguments->service = bb_json_string_equals(json, &value, "SMS") ? @"SMS" : @"iMessage";
    }
    (void)BBRQReadInteger(json, &root, "offset", &arguments->offset);
    (void)BBRQReadInteger(json, &root, "limit", &arguments->limit);
    arguments->hasAfter = BBRQReadInteger(json, &root, "after", &arguments->after);
    arguments->hasBefore = BBRQReadInteger(json, &root, "before", &arguments->before);
    if (bb_json_object_get(json, &root, "sort", &value) && value.type == BBJSONTypeString) {
        arguments->sortAscending = bb_json_string_equals(json, &value, "ASC");
        arguments->sortByLastMessage = bb_json_string_equals(json, &value, "lastmessage");
    }
    BBRQReadBool(json, &root, "withLastMessage", &arguments->withLastMessage);
    BBRQReadBool(json, &root, "withArchived", &arguments->withArchived);
    BBRQReadBool(json, &root, "includeArchived", &arguments->withArchived);
    BBRQReadBool(json, &root, "withAttachments", &arguments->withAttachments);
    BBRQReadBool(json, &root, "withHandle", &arguments->withHandle);
    BBRQReadBool(json, &root, "withChats", &arguments->withChats);
    BBRQReadBool(json, &root, "withChatParticipants", &arguments->withChatParticipants);
    if (arguments->limit < 1) arguments->limit = 1;
    if (arguments->limit > (int64_t)BB_BRIDGE_MAX_LOAD) arguments->limit = BB_BRIDGE_MAX_LOAD;
    if (arguments->offset < 0) arguments->offset = 0;
    return YES;
}

/* A heap copy for blocks that answer after the load delay. Blocks capture
 * structs by value through memcpy, which this target cannot link; a
 * pointer capture with a byte loop avoids that. Free it in the block. */
static BBRQArguments *BBRQCopyArguments(const BBRQArguments *arguments)
{
    BBRQArguments *copy = (BBRQArguments *)malloc(sizeof(*copy));
    volatile unsigned char *out = (volatile unsigned char *)copy;
    const volatile unsigned char *in = (const volatile unsigned char *)arguments;
    if (!copy) return NULL;
    for (size_t i = 0; i < sizeof(*copy); i++) out[i] = in[i];
    return copy;
}

/* ---- replies ------------------------------------------------------------- */

static char *BBRQReplyBuffer(void)
{
    static char *buffer;
    if (!buffer) buffer = (char *)malloc(BB_BRIDGE_REPLY_BYTES);
    return buffer;
}

static void BBRQSendDictionary(NSString *messageName, NSDictionary *userInfo)
{
    CPDistributedMessagingCenter *center = BBBridgeSharedCenter();
    BOOL sent = center && [center sendMessageName:messageName userInfo:userInfo];
    bb_trace(BBTraceMobileSMS, sent ? BBTraceReplySubmitted : BBTraceReplyFailed);
}

static void BBRQSendReply(NSString *requestID, int status, NSString *message,
                          NSString *dataJSON, NSString *metadataJSON, NSString *errorJSON)
{
    NSMutableDictionary *reply = [NSMutableDictionary dictionaryWithCapacity:6];
    if (![requestID length]) return;
    [reply setObject:requestID forKey:@"requestID"];
    [reply setObject:[NSNumber numberWithInt:status] forKey:@"status"];
    [reply setObject:message ?: @"Success" forKey:@"message"];
    if (dataJSON) [reply setObject:dataJSON forKey:@"dataJSON"];
    if (metadataJSON) [reply setObject:metadataJSON forKey:@"metadataJSON"];
    if (errorJSON) [reply setObject:errorJSON forKey:@"errorJSON"];
    bb_trace(BBTraceMobileSMS, BBTraceReplySubmitting);
    BBRQSendDictionary(BBBridgeReplyMessageName, reply);
}

static void BBRQSendError(NSString *requestID, int status, NSString *message,
                          const char *type, const char *detail)
{
    char json[512];
    BBJSONWriter writer;
    size_t length = 0;
    bb_json_writer_init(&writer, json, sizeof(json));
    if (!bb_json_write_raw(&writer, "{\"type\":") || !bb_json_write_string(&writer, type) ||
        !bb_json_write_raw(&writer, ",\"message\":") || !bb_json_write_string(&writer, detail) ||
        !bb_json_write_raw(&writer, "}") || !bb_json_writer_finish(&writer, &length)) return;
    BBRQSendReply(requestID, status, message, nil, nil,
                  [[[NSString alloc] initWithBytes:json length:length
                                          encoding:NSUTF8StringEncoding] autorelease]);
}

static NSString *BBRQStringFromWriter(BBJSONWriter *writer)
{
    size_t length = 0;
    if (!bb_json_writer_finish(writer, &length)) return nil;
    return [[[NSString alloc] initWithBytes:writer->out length:length
                                   encoding:NSUTF8StringEncoding] autorelease];
}

static void BBRQSendTooLarge(NSString *requestID)
{
    BBRQSendError(requestID, 500, @"Server Error", "Server Error", "Reply too large");
}

/* ---- operations ----------------------------------------------------------- */

static void BBRQChatCount(NSString *requestID, const BBRQArguments *arguments)
{
    NSArray *conversations = BBRQConversations();
    NSUInteger imessage = 0;
    NSUInteger sms = 0;
    char json[128];
    BBJSONWriter writer;
    (void)arguments;
    for (id conversation in conversations) {
        NSString *service = BBRQNormalizedService(BBRQServiceName(BBRQChat(conversation)));
        if ([service isEqualToString:@"SMS"]) sms++; else imessage++;
    }
    bb_json_writer_init(&writer, json, sizeof(json));
    if (!bb_json_write_raw(&writer, "{\"total\":") ||
        !bb_json_write_unsigned(&writer, [conversations count]) ||
        !bb_json_write_raw(&writer, ",\"breakdown\":{\"iMessage\":") ||
        !bb_json_write_unsigned(&writer, imessage) ||
        !bb_json_write_raw(&writer, ",\"SMS\":") ||
        !bb_json_write_unsigned(&writer, sms) ||
        !bb_json_write_raw(&writer, "}}")) return;
    BBRQSendReply(requestID, 200, @"Success", BBRQStringFromWriter(&writer), nil, nil);
}

/* Fill the last message of a conversation's chat, if any. */
static BOOL BBRQFillLastMessage(id conversation, const BBChatRecord *chat, NSString *service,
                                BBMessageRecord *record, BBHandleRecord *sender,
                                BBAttachmentRecord *attachments)
{
    id chatObject = BBRQChat(conversation);
    id message = BBRQObject(chatObject, "lastFinishedMessage");
    size_t count = 0;
    if (!message) message = BBRQObject(chatObject, "lastMessage");
    if (!message) return NO;
    return BBRQFillMessage(nil, message, chat, service, record, sender, attachments, &count, YES);
}

static NSArray *BBRQSortedConversations(NSArray *conversations, const BBRQArguments *arguments)
{
    if (!arguments->sortByLastMessage) return conversations;
    return [conversations sortedArrayUsingComparator:^NSComparisonResult(id left, id right) {
        int64_t a = BBRQDateMillis(left, "date");
        int64_t b = BBRQDateMillis(right, "date");
        if (a == b) return NSOrderedSame;
        return a > b ? NSOrderedAscending : NSOrderedDescending;
    }];
}

static void BBRQChatQuery(NSString *requestID, const BBRQArguments *arguments)
{
    NSArray *conversations = BBRQSortedConversations(BBRQConversations(), arguments);
    char *buffer = BBRQReplyBuffer();
    char metadata[128];
    BBJSONWriter writer;
    BBJSONWriter metadataWriter;
    NSUInteger total = [conversations count];
    NSUInteger start = (NSUInteger)arguments->offset;
    NSUInteger emitted = 0;
    if (!buffer) { BBRQSendTooLarge(requestID); return; }
    bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
    if (!bb_json_write_raw(&writer, "[")) { BBRQSendTooLarge(requestID); return; }
    for (NSUInteger index = start; index < total && emitted < (NSUInteger)arguments->limit; index++) {
        @autoreleasepool {
            id conversation = [conversations objectAtIndex:index];
            BBChatRecord chat;
            BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
            BBMessageRecord last;
            BBHandleRecord sender;
            BBAttachmentRecord attachments[BB_BRIDGE_MAX_ATTACHMENTS];
            size_t participantCount = 0;
            if (!BBRQFillChat(conversation, &chat, participants, &participantCount)) continue;
            if (arguments->withLastMessage &&
                BBRQFillLastMessage(conversation, &chat, BBRQServiceName(BBRQChat(conversation)),
                                    &last, &sender, attachments)) {
                chat.lastMessage = &last;
            }
            if (emitted && !bb_json_write_raw(&writer, ",")) { BBRQSendTooLarge(requestID); return; }
            if (!bb_serialize_chat(&writer, &chat, true, arguments->withLastMessage)) {
                BBRQSendTooLarge(requestID);
                return;
            }
            emitted++;
        }
    }
    if (!bb_json_write_raw(&writer, "]")) { BBRQSendTooLarge(requestID); return; }
    bb_json_writer_init(&metadataWriter, metadata, sizeof(metadata));
    (void)bb_serialize_page_metadata(&metadataWriter, arguments->offset, arguments->limit,
                                     (int64_t)total, (int64_t)emitted);
    BBRQSendReply(requestID, 200, @"Success", BBRQStringFromWriter(&writer),
                  BBRQStringFromWriter(&metadataWriter), nil);
}

static void BBRQChatGet(NSString *requestID, const BBRQArguments *arguments)
{
    id conversation = BBRQFindConversation(BBRQConversations(), arguments->guid);
    char *buffer = BBRQReplyBuffer();
    BBJSONWriter writer;
    BBChatRecord chat;
    BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
    BBMessageRecord last;
    BBHandleRecord sender;
    BBAttachmentRecord attachments[BB_BRIDGE_MAX_ATTACHMENTS];
    size_t participantCount = 0;
    if (!conversation || !BBRQFillChat(conversation, &chat, participants, &participantCount)) {
        BBRQSendError(requestID, 404, @"Not Found", "Database Error", "Chat does not exist!");
        return;
    }
    if (!buffer) { BBRQSendTooLarge(requestID); return; }
    if (arguments->withLastMessage &&
        BBRQFillLastMessage(conversation, &chat, BBRQServiceName(BBRQChat(conversation)),
                            &last, &sender, attachments)) chat.lastMessage = &last;
    bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
    if (!bb_serialize_chat(&writer, &chat, true, arguments->withLastMessage)) {
        BBRQSendTooLarge(requestID);
        return;
    }
    BBRQSendReply(requestID, 200, @"Success", BBRQStringFromWriter(&writer), nil, nil);
}

/* One loaded message: conversation + chat item, with its date for sorting. */
static NSDictionary *BBRQEntry(id conversation, id item, int64_t date)
{
    return [NSDictionary dictionaryWithObjectsAndKeys:
            conversation, @"conversation", item, @"item",
            [NSNumber numberWithLongLong:(long long)date], @"date", nil];
}

/* Collect message items of a conversation inside the requested window. */
static void BBRQCollectItems(id conversation, const BBRQArguments *arguments,
                             NSMutableArray *entries)
{
    id chat = BBRQChat(conversation);
    NSArray *items = BBRQArray(chat, "chatItems");
    for (id item in items) {
        int64_t date;
        id message;
        if (!BBRQItemIsMessage(item)) continue;
        message = BBRQObject(item, "message");
        date = BBRQDateMillis(item, "time");
        if (!date) date = BBRQDateMillis(message, "time");
        if (!date) continue;
        if (arguments->hasAfter && date <= arguments->after) continue;
        if (arguments->hasBefore && date >= arguments->before) continue;
        [entries addObject:BBRQEntry(conversation, item, date)];
    }
}

static NSArray *BBRQSortedEntries(NSMutableArray *entries, BOOL ascending)
{
    return [entries sortedArrayUsingComparator:^NSComparisonResult(id left, id right) {
        long long a = [[left objectForKey:@"date"] longLongValue];
        long long b = [[right objectForKey:@"date"] longLongValue];
        if (a == b) return NSOrderedSame;
        if (ascending) return a < b ? NSOrderedAscending : NSOrderedDescending;
        return a > b ? NSOrderedAscending : NSOrderedDescending;
    }];
}

/* Serialize a page of entries as a MessageResponse array. */
static void BBRQSendMessagePage(NSString *requestID, NSArray *sorted,
                                const BBRQArguments *arguments, BOOL withChats)
{
    char *buffer = BBRQReplyBuffer();
    char metadata[128];
    BBJSONWriter writer;
    BBJSONWriter metadataWriter;
    NSUInteger total = [sorted count];
    NSUInteger start = (NSUInteger)arguments->offset;
    NSUInteger emitted = 0;
    if (!buffer) { BBRQSendTooLarge(requestID); return; }
    bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
    if (!bb_json_write_raw(&writer, "[")) { BBRQSendTooLarge(requestID); return; }
    for (NSUInteger index = start; index < total && emitted < (NSUInteger)arguments->limit; index++) {
        @autoreleasepool {
            NSDictionary *entry = [sorted objectAtIndex:index];
            id conversation = [entry objectForKey:@"conversation"];
            id item = [entry objectForKey:@"item"];
            BBChatRecord chat;
            BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
            BBMessageRecord message;
            BBHandleRecord sender;
            BBAttachmentRecord attachments[BB_BRIDGE_MAX_ATTACHMENTS];
            size_t participantCount = 0;
            size_t attachmentCount = 0;
            if (!BBRQFillChat(conversation, &chat, participants, &participantCount)) continue;
            if (!BBRQFillMessage(item, BBRQObject(item, "message"), &chat,
                                 BBRQServiceName(BBRQChat(conversation)), &message, &sender,
                                 attachments, &attachmentCount, arguments->withAttachments)) continue;
            if (emitted && !bb_json_write_raw(&writer, ",")) { BBRQSendTooLarge(requestID); return; }
            if (!bb_serialize_message(&writer, &message, withChats, arguments->withChatParticipants,
                                      arguments->withAttachments, arguments->withHandle)) {
                BBRQSendTooLarge(requestID);
                return;
            }
            emitted++;
        }
    }
    if (!bb_json_write_raw(&writer, "]")) { BBRQSendTooLarge(requestID); return; }
    bb_json_writer_init(&metadataWriter, metadata, sizeof(metadata));
    (void)bb_serialize_page_metadata(&metadataWriter, arguments->offset, arguments->limit,
                                     (int64_t)total, (int64_t)emitted);
    BBRQSendReply(requestID, 200, @"Successfully fetched messages!",
                  BBRQStringFromWriter(&writer), BBRQStringFromWriter(&metadataWriter), nil);
}

static NSUInteger BBRQLoadLimit(const BBRQArguments *arguments)
{
    int64_t wanted = arguments->offset + arguments->limit;
    if (wanted < 25) wanted = 25;
    if (wanted > (int64_t)BB_BRIDGE_MAX_LOAD) wanted = BB_BRIDGE_MAX_LOAD;
    return (NSUInteger)wanted;
}

/* chat.messages: load, then read the items after the proven delay. */
static void BBRQChatMessages(NSString *requestID, const BBRQArguments *arguments)
{
    id conversation = BBRQFindConversation(BBRQConversations(), arguments->guid);
    id chat = BBRQChat(conversation);
    BBRQArguments *copy;
    NSString *stableID = [[requestID copy] autorelease];
    NSString *stableGuid = [[arguments->guid copy] autorelease];
    if (!conversation || !chat) {
        BBRQSendError(requestID, 404, @"Not Found", "Database Error", "Chat does not exist!");
        return;
    }
    if (!BBRQLoadMessages(chat, BBRQLoadLimit(arguments))) {
        BBRQSendError(requestID, 500, @"Server Error", "iMessage Error",
                      "Message loading is unavailable on this device");
        return;
    }
    copy = BBRQCopyArguments(arguments);
    if (!copy) { BBRQSendTooLarge(requestID); return; }
    copy->guid = stableGuid;
    [stableID retain];
    [stableGuid retain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, BB_BRIDGE_LOAD_DELAY_NS),
                   dispatch_get_main_queue(), ^{
        @autoreleasepool {
            @try {
                id refreshed = BBRQFindConversation(BBRQConversations(), stableGuid);
                NSMutableArray *entries = [NSMutableArray array];
                if (!refreshed) {
                    BBRQSendError(stableID, 404, @"Not Found", "Database Error", "Chat does not exist!");
                } else {
                    BBRQCollectItems(refreshed, copy, entries);
                    BBRQSendMessagePage(stableID, BBRQSortedEntries(entries, copy->sortAscending),
                                        copy, NO);
                }
            } @catch (NSException *exception) {
                (void)exception;
                BBRQSendError(stableID, 500, @"Server Error", "iMessage Error",
                              "Message enumeration failed");
            }
            [stableID release];
            [stableGuid release];
            free(copy);
        }
    });
}

/* message.count / message.query: every conversation active in the window. */
static void BBRQMessagesAcrossChats(NSString *requestID, const BBRQArguments *arguments,
                                    BOOL countOnly)
{
    NSArray *conversations = BBRQConversations();
    NSMutableArray *active = [NSMutableArray array];
    BBRQArguments *copy;
    NSString *stableID = [[requestID copy] autorelease];
    NSString *stableGuid = [[arguments->guid copy] autorelease];
    NSUInteger loadLimit = BBRQLoadLimit(arguments);
    for (id conversation in conversations) {
        int64_t activity;
        if ([stableGuid length] && !BBRQConversationMatches(conversation, stableGuid)) continue;
        activity = BBRQDateMillis(conversation, "date");
        if (arguments->hasAfter && activity && activity <= arguments->after) continue;
        if (BBRQLoadMessages(BBRQChat(conversation), loadLimit)) [active addObject:conversation];
    }
    copy = BBRQCopyArguments(arguments);
    if (!copy) { BBRQSendTooLarge(requestID); return; }
    copy->guid = stableGuid;
    [stableID retain];
    [stableGuid retain];
    [active retain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, BB_BRIDGE_LOAD_DELAY_NS),
                   dispatch_get_main_queue(), ^{
        @autoreleasepool {
            @try {
                NSMutableArray *entries = [NSMutableArray array];
                for (id conversation in active) BBRQCollectItems(conversation, copy, entries);
                if (countOnly) {
                    char json[64];
                    BBJSONWriter writer;
                    bb_json_writer_init(&writer, json, sizeof(json));
                    if (bb_json_write_raw(&writer, "{\"total\":") &&
                        bb_json_write_unsigned(&writer, [entries count]) &&
                        bb_json_write_raw(&writer, "}")) {
                        BBRQSendReply(stableID, 200, @"Success", BBRQStringFromWriter(&writer), nil, nil);
                    }
                } else {
                    BBRQSendMessagePage(stableID, BBRQSortedEntries(entries, copy->sortAscending),
                                        copy, copy->withChats);
                }
            } @catch (NSException *exception) {
                (void)exception;
                BBRQSendError(stableID, 500, @"Server Error", "iMessage Error",
                              "Message enumeration failed");
            }
            [stableID release];
            [stableGuid release];
            free(copy);
            [active release];
        }
    });
}

/* message.get: only messages already loaded in memory are visible. */
static void BBRQMessageGet(NSString *requestID, const BBRQArguments *arguments)
{
    NSArray *conversations = BBRQConversations();
    char *buffer = BBRQReplyBuffer();
    if (![arguments->guid length] || !buffer) {
        BBRQSendError(requestID, 404, @"Not Found", "Database Error", "Message does not exist!");
        return;
    }
    for (id conversation in conversations) {
        NSArray *items = BBRQArray(BBRQChat(conversation), "chatItems");
        for (id item in items) {
            id message;
            NSString *guid;
            if (!BBRQItemIsMessage(item)) continue;
            message = BBRQObject(item, "message");
            guid = BBRQString(message, "guid");
            if (![guid isEqualToString:arguments->guid]) continue;
            {
                BBChatRecord chat;
                BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
                BBMessageRecord record;
                BBHandleRecord sender;
                BBAttachmentRecord attachments[BB_BRIDGE_MAX_ATTACHMENTS];
                size_t participantCount = 0;
                size_t attachmentCount = 0;
                BBJSONWriter writer;
                if (!BBRQFillChat(conversation, &chat, participants, &participantCount) ||
                    !BBRQFillMessage(item, message, &chat, BBRQServiceName(BBRQChat(conversation)),
                                     &record, &sender, attachments, &attachmentCount, YES)) break;
                bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
                if (!bb_serialize_message(&writer, &record, arguments->withChats,
                                          arguments->withChatParticipants,
                                          arguments->withAttachments, arguments->withHandle)) {
                    BBRQSendTooLarge(requestID);
                    return;
                }
                BBRQSendReply(requestID, 200, @"Success", BBRQStringFromWriter(&writer), nil, nil);
                return;
            }
        }
    }
    BBRQSendError(requestID, 404, @"Not Found", "Database Error", "Message does not exist!");
}

static void BBRQAttachmentGet(NSString *requestID, const BBRQArguments *arguments)
{
    id transfer = BBRQTransferForGUID(arguments->guid);
    BBAttachmentRecord record;
    char json[2048];
    BBJSONWriter writer;
    if (!transfer || !BBRQFillAttachment(arguments->guid, transfer, &record)) {
        BBRQSendError(requestID, 404, @"Not Found", "Database Error", "Attachment does not exist!");
        return;
    }
    bb_json_writer_init(&writer, json, sizeof(json));
    if (!bb_serialize_attachment(&writer, &record)) { BBRQSendTooLarge(requestID); return; }
    BBRQSendReply(requestID, 200, @"Success", BBRQStringFromWriter(&writer), nil, nil);
}

/* Resolve a downloaded attachment's local file path and hand it to the
 * SpringBoard side to stream. iCloud/undownloaded transfers have no local
 * file and get a truthful 404. No bytes cross IPC. */
static void BBRQAttachmentDownload(NSString *requestID, const BBRQArguments *arguments)
{
    id transfer = BBRQTransferForGUID(arguments->guid);
    id url;
    NSString *path = nil;
    NSString *mime;
    if (!transfer) {
        BBRQSendError(requestID, 404, @"Not Found", "Database Error", "Attachment does not exist!");
        return;
    }
    url = BBRQObject(transfer, "localURL");
    if ([url isKindOfClass:[NSURL class]]) path = [(NSURL *)url path];
    if (![path length]) path = BBRQString(transfer, "localPath");
    if (![path length] || ![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        BBRQSendError(requestID, 404, @"Not Found", "Database Error",
                      "Attachment has not been downloaded to this device");
        return;
    }
    mime = BBRQString(transfer, "mimeType");
    {
        NSMutableDictionary *reply = [NSMutableDictionary dictionaryWithCapacity:5];
        [reply setObject:requestID forKey:@"requestID"];
        [reply setObject:[NSNumber numberWithInt:200] forKey:@"status"];
        [reply setObject:@"Success" forKey:@"message"];
        [reply setObject:path forKey:@"filePath"];
        if ([mime length]) [reply setObject:mime forKey:@"contentType"];
        bb_trace(BBTraceMobileSMS, BBTraceReplySubmitting);
        BBRQSendDictionary(BBBridgeReplyMessageName, reply);
    }
}

/* Send a text into an existing chat via the device-verified ChatKit
 * composition path (initWithText:subject: -> messageWithComposition: ->
 * sendMessage:newComposition:). Replies with a minimal MessageResponse
 * echoing tempGuid so the client reconciles its optimistic bubble. */
/* An empty text composition: the base every ChatKit send starts from. */
static id BBRQEmptyComposition(NSString *text)
{
    Class compClass = NSClassFromString(@"CKComposition");
    SEL initTextSel = sel_registerName("initWithText:subject:");
    NSAttributedString *attr = [[[NSAttributedString alloc] initWithString:text ?: @""] autorelease];
    id raw;
    if (!compClass) return nil;
    raw = [compClass alloc];
    if (![raw respondsToSelector:initTextSel]) {
        [raw release];
        return nil;
    }
    return [((id (*)(id, SEL, id, id))objc_msgSend)(raw, initTextSel, attr, nil) autorelease];
}

/* Send a composition through a ChatKit conversation. Returns the IMMessage
 * (its guid, or tempGuid, then a synthesized id, through *guidOut) or nil
 * with *problem set. Runs on the main queue like every ChatKit call. */
static id BBRQSendComposition(id conversation, id composition, NSString *tempGuid,
                              NSString **guidOut, const char **problem)
{
    SEL msgWithCompSel = sel_registerName("messageWithComposition:");
    SEL sendSel = sel_registerName("sendMessage:newComposition:");
    id message;
    NSString *guid;
    *problem = NULL;
    *guidOut = nil;
    if (!composition || ![conversation respondsToSelector:msgWithCompSel] ||
        ![conversation respondsToSelector:sendSel]) {
        *problem = "Composition is unavailable";
        return nil;
    }
    message = ((id (*)(id, SEL, id))objc_msgSend)(conversation, msgWithCompSel, composition);
    if (!message) {
        *problem = "Message composition failed";
        return nil;
    }
    ((void (*)(id, SEL, id, BOOL))objc_msgSend)(conversation, sendSel, message, YES);
    bb_trace(BBTraceMobileSMS, BBTraceSendDispatched);
    guid = BBRQString(message, "guid");
    if (![guid length]) guid = tempGuid;
    if (![guid length]) guid = [NSString stringWithFormat:@"bb-sent-%llu",
                                (unsigned long long)([[NSDate date] timeIntervalSince1970] * 1000.0)];
    *guidOut = guid;
    return message;
}

/* Compose and send one text. Returns the message guid or nil with *problem. */
static NSString *BBRQComposeAndSend(id conversation, NSString *text, NSString *tempGuid,
                                    const char **problem)
{
    NSString *guid = nil;
    id composition = BBRQEmptyComposition(text);
    if (!composition) {
        *problem = "Composition is unavailable";
        return nil;
    }
    (void)BBRQSendComposition(conversation, composition, tempGuid, &guid, problem);
    return guid;
}

/* Minimal echo of a just-sent text so the client reconciles by
 * tempGuid+guid. Every field set explicitly (no memset: the bridge target
 * cannot link compiler-emitted libc). */
static void BBRQFillSentMessage(BBMessageRecord *record, NSString *guid, NSString *tempGuid,
                                NSString *text, const BBChatRecord *chat)
{
    record->guid = [guid UTF8String];
    record->tempGuid = [tempGuid length] ? [tempGuid UTF8String] : NULL;
    record->text = [text UTF8String];
    record->subject = NULL;
    record->handle = NULL;
    record->dateCreated = (int64_t)([[NSDate date] timeIntervalSince1970] * 1000.0);
    record->dateRead = 0;
    record->dateDelivered = 0;
    record->isFromMe = YES;
    record->isDelivered = YES;
    record->error = 0;
    record->itemType = 0;
    record->groupTitle = NULL;
    record->groupActionType = 0;
    record->attachments = NULL;
    record->attachmentCount = 0;
    record->chat = chat;
}

static void BBRQMessageSend(NSString *requestID, const BBRQArguments *arguments)
{
    id conversation = BBRQFindConversation(BBRQConversations(), arguments->guid);
    BBChatRecord chat;
    BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
    size_t participantCount = 0;
    if (![arguments->message length] || ![arguments->tempGuid length]) {
        BBRQSendError(requestID, 400, @"Bad Request", "Validation Error", "Missing message or tempGuid");
        return;
    }
    if (!conversation || !BBRQFillChat(conversation, &chat, participants, &participantCount)) {
        BBRQSendError(requestID, 404, @"Not Found", "Database Error", "Chat does not exist!");
        return;
    }
    @try {
        const char *problem = NULL;
        NSString *guid = BBRQComposeAndSend(conversation, arguments->message, arguments->tempGuid, &problem);
        BBMessageRecord record;
        char *buffer = BBRQReplyBuffer();
        BBJSONWriter writer;
        if (!guid) {
            BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", problem);
            return;
        }
        if (!buffer) { BBRQSendReply(requestID, 200, @"Message sent!", nil, nil, nil); return; }
        BBRQFillSentMessage(&record, guid, arguments->tempGuid, arguments->message, &chat);
        bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
        if (bb_serialize_message(&writer, &record, YES, YES, NO, NO)) {
            BBRQSendReply(requestID, 200, @"Message sent!", BBRQStringFromWriter(&writer), nil, nil);
        } else {
            BBRQSendReply(requestID, 200, @"Message sent!", nil, nil, nil);
        }
    } @catch (NSException *exception) {
        (void)exception;
        bb_trace(BBTraceMobileSMS, BBTraceSendFailed);
        BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", "Send failed");
    }
}

/* The IMCore account for a service name, falling back to the other
 * service when the requested one has no account (an iPad with no SMS
 * relay, or iMessage signed out). The resulting chat reports the service
 * it was actually created on. */
static id BBRQAccountForService(NSString *requested)
{
    Class serviceClass = NSClassFromString(@"IMService");
    Class controllerClass = NSClassFromString(@"IMAccountController");
    id controller = controllerClass ? BBRQObject((id)controllerClass, "sharedInstance") : nil;
    BOOL wantSMS = [requested isEqualToString:@"SMS"];
    const char *order[2];
    order[0] = wantSMS ? "smsService" : "iMessageService";
    order[1] = wantSMS ? "iMessageService" : "smsService";
    if (!serviceClass || !controller) return nil;
    for (unsigned i = 0; i < 2; i++) {
        id service = BBRQObject((id)serviceClass, order[i]);
        id account = service ? BBRQObjectWithArgument(controller, "bestAccountForService:", service) : nil;
        if (account) return account;
    }
    return nil;
}

/* chat.new: resolve (or create) the chat for a set of addresses and send
 * its first message. The chain is the one device-verified on iOS 9.3.5:
 * IMAccountController bestAccountForService: → IMAccount imHandleWithID: →
 * IMChatRegistry chatForIMHandle(s): → IMChat chatIdentifier →
 * CKConversationList conversationForExistingChatWithGroupID:. A message is
 * mandatory (the router enforces it) so the chat becomes visible in the
 * conversation list. Sending is a real outbound action. */
static void BBRQChatNew(NSString *requestID, const BBRQArguments *arguments)
{
    NSArray *addresses = arguments->addresses;
    if (![addresses count] || [addresses count] > BB_BRIDGE_MAX_PARTICIPANTS ||
        ![arguments->message length]) {
        BBRQSendError(requestID, 400, @"Bad Request", "Validation Error", "Missing addresses or message");
        return;
    }
    bb_trace(BBTraceMobileSMS, BBTraceNewChatResolving);
    @try {
        id account = BBRQAccountForService(arguments->service);
        Class registryClass = NSClassFromString(@"IMChatRegistry");
        id registry = registryClass ? BBRQObject((id)registryClass, "sharedInstance") : nil;
        NSMutableArray *handles = [NSMutableArray arrayWithCapacity:[addresses count]];
        id chat;
        NSString *identifier;
        id conversation;
        BBChatRecord chatRecord;
        BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
        size_t participantCount = 0;
        const char *problem = NULL;
        NSString *guid;
        BBMessageRecord record;
        char *buffer;
        BBJSONWriter writer;

        if (!account || !registry) {
            bb_trace(BBTraceMobileSMS, BBTraceNewChatFailed);
            BBRQSendError(requestID, 500, @"Server Error", "iMessage Error",
                          "No messaging account is available");
            return;
        }
        for (NSString *address in addresses) {
            id handle = BBRQObjectWithArgument(account, "imHandleWithID:", address);
            if (!handle) {
                bb_trace(BBTraceMobileSMS, BBTraceNewChatFailed);
                BBRQSendError(requestID, 400, @"Bad Request", "Validation Error",
                              "An address could not be resolved to a handle");
                return;
            }
            [handles addObject:handle];
        }
        chat = [handles count] == 1 ?
            BBRQObjectWithArgument(registry, "chatForIMHandle:", [handles objectAtIndex:0]) :
            BBRQObjectWithArgument(registry, "chatForIMHandles:", handles);
        identifier = BBRQString(chat, "chatIdentifier");
        if (![identifier length]) identifier = BBRQString(chat, "persistentID");
        conversation = BBRQConversationForIdentifier(identifier);
        if (!conversation || !BBRQFillChat(conversation, &chatRecord, participants, &participantCount)) {
            bb_trace(BBTraceMobileSMS, BBTraceNewChatFailed);
            BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", "Failed to create chat");
            return;
        }
        bb_trace(BBTraceMobileSMS, BBTraceNewChatResolved);

        guid = BBRQComposeAndSend(conversation, arguments->message, arguments->tempGuid, &problem);
        if (!guid) {
            BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", problem);
            return;
        }
        /* The message is out: never answer with an error from here on, or
         * the client would retry and send it twice. */
        buffer = BBRQReplyBuffer();
        if (!buffer) { BBRQSendReply(requestID, 200, @"Successfully created chat!", nil, nil, nil); return; }
        BBRQFillSentMessage(&record, guid, arguments->tempGuid, arguments->message, &chatRecord);
        bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
        if (bb_serialize_chat_with_messages(&writer, &chatRecord, &record, 1)) {
            BBRQSendReply(requestID, 200, @"Successfully created chat!", BBRQStringFromWriter(&writer), nil, nil);
        } else {
            BBRQSendReply(requestID, 200, @"Successfully created chat!", nil, nil, nil);
        }
    } @catch (NSException *exception) {
        (void)exception;
        bb_trace(BBTraceMobileSMS, BBTraceNewChatFailed);
        BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", "Chat creation failed");
    }
}

/* Staged uploads live under this prefix (written by SpringBoard, mode
 * 0600, same user). The bridge deletes the file once the send has been
 * dispatched, or on any failure before that. */
static NSString * const BBRQUploadPrefix = @"/tmp/bbupload-";

static void BBRQDiscardUpload(NSString *path)
{
    if ([path hasPrefix:BBRQUploadPrefix]) unlink([path fileSystemRepresentation]);
}

/* message.attachment: send a staged file through the ChatKit media path
 * verified on the reference device (CKMediaObjectManager sharedInstance →
 * mediaObjectWithFileURL:filename:transcoderUserInfo:, then
 * compositionByAppendingMediaObject: on an empty composition). The reply
 * echoes tempGuid and the transfer record(s) ChatKit created. */
static void BBRQMessageAttachment(NSString *requestID, const BBRQArguments *arguments)
{
    NSString *path = arguments->uploadPath;
    NSString *name = [arguments->name length] ? arguments->name : [path lastPathComponent];
    id conversation;
    BBChatRecord chat;
    BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
    size_t participantCount = 0;
    struct stat info;

    if (![path hasPrefix:BBRQUploadPrefix] || [path rangeOfString:@"/.."].location != NSNotFound ||
        ![arguments->tempGuid length]) {
        BBRQSendError(requestID, 400, @"Bad Request", "Validation Error", "Missing upload or tempGuid");
        BBRQDiscardUpload(path);
        return;
    }
    if (stat([path fileSystemRepresentation], &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        BBRQSendError(requestID, 400, @"Bad Request", "Validation Error", "Staged attachment is missing");
        BBRQDiscardUpload(path);
        return;
    }
    conversation = BBRQFindConversation(BBRQConversations(), arguments->guid);
    if (!conversation || !BBRQFillChat(conversation, &chat, participants, &participantCount)) {
        BBRQSendError(requestID, 404, @"Not Found", "Database Error", "Chat does not exist!");
        BBRQDiscardUpload(path);
        return;
    }
    @try {
        Class momClass = NSClassFromString(@"CKMediaObjectManager");
        id manager = momClass ? BBRQObject((id)momClass, "sharedInstance") : nil;
        NSURL *url = [NSURL fileURLWithPath:path];
        id media = manager ? BBRQObjectWithThreeArguments(manager,
            "mediaObjectWithFileURL:filename:transcoderUserInfo:", url, name, nil) : nil;
        id base = media ? BBRQEmptyComposition(@"") : nil;
        id composition = base ? BBRQObjectWithArgument(base, "compositionByAppendingMediaObject:", media) : nil;
        const char *problem = NULL;
        NSString *guid = nil;
        id message;
        BBMessageRecord record;
        BBAttachmentRecord attachments[BB_BRIDGE_MAX_ATTACHMENTS];
        size_t attachmentCount = 0;
        char *buffer;
        BBJSONWriter writer;

        if (!media) {
            BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", "Media object creation failed");
            BBRQDiscardUpload(path);
            return;
        }
        message = BBRQSendComposition(conversation, composition, arguments->tempGuid, &guid, &problem);
        if (!message) {
            BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", problem);
            BBRQDiscardUpload(path);
            return;
        }
        /* ChatKit has copied the bytes into its own transfer; the staged
         * file is no longer needed (deleting it here is device-verified). */
        BBRQDiscardUpload(path);

        /* Nothing below may answer with an error: the attachment is out. */
        buffer = BBRQReplyBuffer();
        if (!buffer) { BBRQSendReply(requestID, 200, @"Attachment sent!", nil, nil, nil); return; }
        BBRQFillSentMessage(&record, guid, arguments->tempGuid, @"", &chat);
        {
            NSArray *guids = BBRQArray(message, "fileTransferGUIDs");
            for (NSString *transferGuid in guids) {
                if (attachmentCount >= BB_BRIDGE_MAX_ATTACHMENTS) break;
                if (![transferGuid isKindOfClass:[NSString class]]) continue;
                if (BBRQFillAttachment(transferGuid, BBRQTransferForGUID(transferGuid),
                                       &attachments[attachmentCount])) attachmentCount++;
            }
        }
        record.attachments = attachments;
        record.attachmentCount = attachmentCount;
        bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
        if (bb_serialize_message(&writer, &record, YES, YES, YES, NO)) {
            BBRQSendReply(requestID, 200, @"Attachment sent!", BBRQStringFromWriter(&writer), nil, nil);
        } else {
            BBRQSendReply(requestID, 200, @"Attachment sent!", nil, nil, nil);
        }
    } @catch (NSException *exception) {
        (void)exception;
        bb_trace(BBTraceMobileSMS, BBTraceSendFailed);
        BBRQDiscardUpload(path);
        BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", "Attachment send failed");
    }
}

/* AddressBook is deprecated in iOS 9 (superseded by the Contacts framework)
 * but fully functional on 9.3.5, and it is the API Messages itself uses.
 * Suppress the deprecation error for this self-contained block. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Copy a CFString into a UTF-8 stack buffer; empty on NULL/failure. */
static void BBRQCFToUTF8(CFStringRef value, char *out, size_t capacity)
{
    out[0] = '\0';
    if (value) (void)CFStringGetCString(value, out, (CFIndex)capacity, kCFStringEncodingUTF8);
}

#define BB_BRIDGE_MAX_CONTACT_VALUES 8
#define BB_BRIDGE_CONTACT_JSON_SOFT_CAP 3500000  /* file-staged, so bounded by the 4MB reply buffer, not the connection scratch */

/* Read the device AddressBook and return ContactResponse records (names,
 * phone numbers, emails, and base64 thumbnail avatars). The full response
 * envelope is staged to a private 0600 temp file that the SpringBoard
 * transport streams and then unlinks: contacts with avatars exceed the
 * per-connection response buffer, and no bytes cross IPC. All CoreFoundation
 * results are released explicitly (MRC). */
static void BBRQContactQuery(NSString *requestID, const BBRQArguments *arguments)
{
    char *buffer = BBRQReplyBuffer();
    BBJSONWriter writer;
    ABAddressBookRef book = ABAddressBookCreateWithOptions(NULL, NULL);
    CFArrayRef people = book ? ABAddressBookCopyArrayOfAllPeople(book) : NULL;
    CFIndex count = people ? CFArrayGetCount(people) : 0;
    size_t emitted = 0;
    size_t length = 0;
    NSString *path;
    const char *pathBytes;
    int fd;
    (void)arguments;
    if (!buffer) {
        if (people) CFRelease(people);
        if (book) CFRelease(book);
        BBRQSendTooLarge(requestID);
        return;
    }
    bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
    (void)bb_json_write_raw(&writer, "{\"status\":200,\"message\":\"Success\",\"data\":[");
    for (CFIndex i = 0; i < count; i++) {
        @autoreleasepool {
            ABRecordRef person = (ABRecordRef)CFArrayGetValueAtIndex(people, i);
            char identifier[32];
            char displayName[256];
            char firstName[128];
            char lastName[128];
            char phoneAddr[BB_BRIDGE_MAX_CONTACT_VALUES][80];
            char phoneLabel[BB_BRIDGE_MAX_CONTACT_VALUES][40];
            char emailAddr[BB_BRIDGE_MAX_CONTACT_VALUES][128];
            char emailLabel[BB_BRIDGE_MAX_CONTACT_VALUES][40];
            BBContactAddress phones[BB_BRIDGE_MAX_CONTACT_VALUES];
            BBContactAddress emails[BB_BRIDGE_MAX_CONTACT_VALUES];
            BBContactRecord record;
            CFStringRef composite;
            CFStringRef first;
            CFStringRef last;
            ABMultiValueRef phoneValues;
            ABMultiValueRef emailValues;
            CFDataRef imageData;
            NSString *avatar = nil;
            size_t phoneCount = 0;
            size_t emailCount = 0;

            if (writer.length > BB_BRIDGE_CONTACT_JSON_SOFT_CAP || !person) {
                if (!person) continue;
                break;
            }
            snprintf(identifier, sizeof(identifier), "ab-%d", (int)ABRecordGetRecordID(person));
            composite = ABRecordCopyCompositeName(person);
            first = (CFStringRef)ABRecordCopyValue(person, kABPersonFirstNameProperty);
            last = (CFStringRef)ABRecordCopyValue(person, kABPersonLastNameProperty);
            BBRQCFToUTF8(composite, displayName, sizeof(displayName));
            BBRQCFToUTF8(first, firstName, sizeof(firstName));
            BBRQCFToUTF8(last, lastName, sizeof(lastName));
            if (composite) CFRelease(composite);
            if (first) CFRelease(first);
            if (last) CFRelease(last);

            phoneValues = (ABMultiValueRef)ABRecordCopyValue(person, kABPersonPhoneProperty);
            if (phoneValues) {
                CFIndex pc = ABMultiValueGetCount(phoneValues);
                for (CFIndex j = 0; j < pc && phoneCount < BB_BRIDGE_MAX_CONTACT_VALUES; j++) {
                    CFStringRef v = (CFStringRef)ABMultiValueCopyValueAtIndex(phoneValues, j);
                    CFStringRef rawLabel = ABMultiValueCopyLabelAtIndex(phoneValues, j);
                    CFStringRef label = rawLabel ? ABAddressBookCopyLocalizedLabel(rawLabel) : NULL;
                    BBRQCFToUTF8(v, phoneAddr[phoneCount], sizeof(phoneAddr[phoneCount]));
                    BBRQCFToUTF8(label, phoneLabel[phoneCount], sizeof(phoneLabel[phoneCount]));
                    if (v) CFRelease(v);
                    if (rawLabel) CFRelease(rawLabel);
                    if (label) CFRelease(label);
                    if (phoneAddr[phoneCount][0]) {
                        phones[phoneCount].address = phoneAddr[phoneCount];
                        phones[phoneCount].label = phoneLabel[phoneCount];
                        phoneCount++;
                    }
                }
                CFRelease(phoneValues);
            }
            emailValues = (ABMultiValueRef)ABRecordCopyValue(person, kABPersonEmailProperty);
            if (emailValues) {
                CFIndex ec = ABMultiValueGetCount(emailValues);
                for (CFIndex j = 0; j < ec && emailCount < BB_BRIDGE_MAX_CONTACT_VALUES; j++) {
                    CFStringRef v = (CFStringRef)ABMultiValueCopyValueAtIndex(emailValues, j);
                    CFStringRef rawLabel = ABMultiValueCopyLabelAtIndex(emailValues, j);
                    CFStringRef label = rawLabel ? ABAddressBookCopyLocalizedLabel(rawLabel) : NULL;
                    BBRQCFToUTF8(v, emailAddr[emailCount], sizeof(emailAddr[emailCount]));
                    BBRQCFToUTF8(label, emailLabel[emailCount], sizeof(emailLabel[emailCount]));
                    if (v) CFRelease(v);
                    if (rawLabel) CFRelease(rawLabel);
                    if (label) CFRelease(label);
                    if (emailAddr[emailCount][0]) {
                        emails[emailCount].address = emailAddr[emailCount];
                        emails[emailCount].label = emailLabel[emailCount];
                        emailCount++;
                    }
                }
                CFRelease(emailValues);
            }

            if (!displayName[0] && !firstName[0] && !lastName[0] && !phoneCount && !emailCount) continue;

            imageData = ABPersonCopyImageDataWithFormat(person, kABPersonImageFormatThumbnail);
            if (imageData) {
                avatar = [(NSData *)imageData base64EncodedStringWithOptions:0];
                CFRelease(imageData);
            }

            record.identifier = identifier;
            record.displayName = displayName[0] ? displayName : NULL;
            record.firstName = firstName[0] ? firstName : NULL;
            record.lastName = lastName[0] ? lastName : NULL;
            record.phoneNumbers = phones;
            record.phoneCount = phoneCount;
            record.emails = emails;
            record.emailCount = emailCount;
            record.avatarBase64 = [avatar length] ? [avatar UTF8String] : NULL;
            if (emitted && !bb_json_write_raw(&writer, ",")) break;
            if (!bb_serialize_contact(&writer, &record)) break;
            emitted++;
        }
    }
    if (people) CFRelease(people);
    if (book) CFRelease(book);
    if (!bb_json_write_raw(&writer, "]}") || !bb_json_writer_finish(&writer, &length)) {
        BBRQSendTooLarge(requestID);
        return;
    }

    /* Stage the envelope to a private 0600 file the transport streams then
     * unlinks. */
    path = [NSString stringWithFormat:@"/tmp/bbresp-%@.json", requestID];
    pathBytes = [path fileSystemRepresentation];
    fd = pathBytes ? open(pathBytes, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600) : -1;
    if (fd < 0) {
        BBRQSendError(requestID, 500, @"Server Error", "Server Error", "Could not stage contacts");
        return;
    }
    {
        size_t written = 0;
        BOOL ok = YES;
        while (written < length) {
            ssize_t n = write(fd, buffer + written, length - written);
            if (n > 0) { written += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            ok = NO; break;
        }
        close(fd);
        if (!ok) {
            unlink(pathBytes);
            BBRQSendError(requestID, 500, @"Server Error", "Server Error", "Could not stage contacts");
            return;
        }
    }
    {
        NSMutableDictionary *reply = [NSMutableDictionary dictionaryWithCapacity:6];
        [reply setObject:requestID forKey:@"requestID"];
        [reply setObject:[NSNumber numberWithInt:200] forKey:@"status"];
        [reply setObject:@"Success" forKey:@"message"];
        [reply setObject:path forKey:@"filePath"];
        [reply setObject:@"application/json; charset=utf-8" forKey:@"contentType"];
        [reply setObject:[NSNumber numberWithBool:YES] forKey:@"ephemeral"];
        bb_trace(BBTraceMobileSMS, BBTraceReplySubmitting);
        BBRQSendDictionary(BBBridgeReplyMessageName, reply);
    }
}

#pragma clang diagnostic pop

/* ---- request dispatch ------------------------------------------------------ */

static void BBRQHandleRequest(NSDictionary *userInfo)
{
    NSString *requestID = [userInfo objectForKey:@"requestID"];
    NSString *operation = [userInfo objectForKey:@"operation"];
    NSString *argumentsJSON = [userInfo objectForKey:@"argumentsJSON"];
    BBRQArguments arguments;

    if (![requestID isKindOfClass:[NSString class]] || ![requestID length] ||
        ![operation isKindOfClass:[NSString class]]) return;
    if (![argumentsJSON isKindOfClass:[NSString class]] ||
        !BBRQParseArguments(argumentsJSON, &arguments)) {
        BBRQSendError(requestID, 400, @"Bad Request", "Validation Error", "Malformed request arguments");
        return;
    }
    @try {
        if ([operation isEqualToString:@"chat.count"]) BBRQChatCount(requestID, &arguments);
        else if ([operation isEqualToString:@"chat.query"]) BBRQChatQuery(requestID, &arguments);
        else if ([operation isEqualToString:@"chat.get"]) BBRQChatGet(requestID, &arguments);
        else if ([operation isEqualToString:@"chat.messages"]) BBRQChatMessages(requestID, &arguments);
        else if ([operation isEqualToString:@"message.count"]) BBRQMessagesAcrossChats(requestID, &arguments, YES);
        else if ([operation isEqualToString:@"message.query"]) BBRQMessagesAcrossChats(requestID, &arguments, NO);
        else if ([operation isEqualToString:@"message.get"]) BBRQMessageGet(requestID, &arguments);
        else if ([operation isEqualToString:@"attachment.get"]) BBRQAttachmentGet(requestID, &arguments);
        else if ([operation isEqualToString:@"attachment.download"]) BBRQAttachmentDownload(requestID, &arguments);
        else if ([operation isEqualToString:@"message.send"]) BBRQMessageSend(requestID, &arguments);
        else if ([operation isEqualToString:@"contact.query"]) BBRQContactQuery(requestID, &arguments);
        else if ([operation isEqualToString:@"chat.new"]) BBRQChatNew(requestID, &arguments);
        else if ([operation isEqualToString:@"message.attachment"]) BBRQMessageAttachment(requestID, &arguments);
        else BBRQSendError(requestID, 400, @"Bad Request", "Validation Error", "Unsupported operation");
    } @catch (NSException *exception) {
        (void)exception;
        bb_trace(BBTraceMobileSMS, BBTraceEnumerationException);
        BBRQSendError(requestID, 500, @"Server Error", "iMessage Error", "Operation failed");
    }
}

@interface BBBridgeRequestReceiver : NSObject
- (void)handleRequest:(NSNotification *)notification;
@end

@implementation BBBridgeRequestReceiver

- (void)handleRequest:(NSNotification *)notification
{
    NSDictionary *userInfo = [[notification userInfo] copy];
    if (![userInfo isKindOfClass:[NSDictionary class]]) {
        [userInfo release];
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            BBRQHandleRequest(userInfo);
            [userInfo release];
        }
    });
}

@end

static BBBridgeRequestReceiver *BBRQReceiver;

void BBBridgeInstallTypedRequests(void)
{
    if (BBRQReceiver) return;
    BBRQReceiver = [[BBBridgeRequestReceiver alloc] init];
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:BBRQReceiver selector:@selector(handleRequest:)
               name:BBBridgeRequestNotificationName object:nil];
}

/* ---- events ------------------------------------------------------------------ */

static NSMutableDictionary *BBRQLastEmitted(void)
{
    static NSMutableDictionary *table;
    if (!table) table = [[NSMutableDictionary alloc] init];
    return table;
}

/* Serialize the last message of the target conversation and send it as the
 * named event. dedupe keys the emission on (guid, delivered, read, error). */
static void BBRQEmitLastMessage(NSString *target, NSString *eventName, BOOL dedupe)
{
    @autoreleasepool {
        @try {
            id conversation = BBRQFindConversation(BBRQConversations(), target);
            BBChatRecord chat;
            BBHandleRecord participants[BB_BRIDGE_MAX_PARTICIPANTS];
            BBMessageRecord message;
            BBHandleRecord sender;
            BBAttachmentRecord attachments[BB_BRIDGE_MAX_ATTACHMENTS];
            size_t participantCount = 0;
            char *buffer = BBRQReplyBuffer();
            BBJSONWriter writer;
            NSString *payload;
            NSString *state;
            if (!conversation || !buffer) return;
            if (!BBRQFillChat(conversation, &chat, participants, &participantCount)) return;
            if (!BBRQFillLastMessage(conversation, &chat, BBRQServiceName(BBRQChat(conversation)),
                                     &message, &sender, attachments)) return;
            state = [NSString stringWithFormat:@"%s|%d|%lld|%lld|%d", message.guid,
                     (int)message.isDelivered, (long long)message.dateRead,
                     (long long)message.dateDelivered, message.error];
            if (dedupe && [[[BBRQLastEmitted() objectForKey:target] description] isEqualToString:state])
                return;
            [BBRQLastEmitted() setObject:state forKey:target];
            bb_json_writer_init(&writer, buffer, BB_BRIDGE_REPLY_BYTES);
            if (!bb_serialize_message(&writer, &message, true, true, true, true)) return;
            payload = BBRQStringFromWriter(&writer);
            if (!payload) return;
            BBRQSendDictionary(BBBridgeEventMessageName,
                               [NSDictionary dictionaryWithObjectsAndKeys:
                                eventName, @"event", payload, @"payloadJSON", nil]);
        } @catch (NSException *exception) {
            (void)exception;
        }
    }
}

void BBBridgeEmitNewMessageForTarget(NSString *target)
{
    if (![target length]) return;
    BBRQEmitLastMessage(target, @"new-message", NO);
}

void BBBridgeEmitUpdatedMessageForTarget(NSString *target)
{
    if (![target length]) return;
    BBRQEmitLastMessage(target, @"updated-message", YES);
}
