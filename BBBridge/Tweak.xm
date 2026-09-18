#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

#import <AppSupport/CPDistributedMessagingCenter.h>
#import <rocketbootstrap/rocketbootstrap.h>

#import "BBBridgeRuntime.h"
#import "BBBridgeRequests.h"
#import "BBTrace.h"

/* MobileSMS side. Everything that touches IMCore/ChatKit lives in
 * BBBridgeRequests.xm and runs on the main queue; this file only connects
 * to SpringBoard's messaging center and forwards the two ChatKit
 * notifications that mean "something changed" into typed events. */

static NSString * const BBServerCenterName = @"com.bluebubbles.bridge";
static CPDistributedMessagingCenter *BBBridgeCenter;

CPDistributedMessagingCenter *BBBridgeSharedCenter(void)
{
    return BBBridgeCenter;
}

/* Signature-checked object-returning call; nil when the receiver does not
 * respond or the selector does not return an object. */
static id BBBridgeObject(id receiver, SEL selector)
{
    NSMethodSignature *signature;
    const char *type;
    if (!receiver || ![receiver respondsToSelector:selector]) return nil;
    signature = [receiver methodSignatureForSelector:selector];
    if (!signature || [signature numberOfArguments] != 2) return nil;
    type = [signature methodReturnType];
    while (*type == 'r' || *type == 'n' || *type == 'N' || *type == 'o' ||
           *type == 'O' || *type == 'R' || *type == 'V') type++;
    if (*type != '@') return nil;
    return ((id (*)(id, SEL))objc_msgSend)(receiver, selector);
}

static NSString *BBBridgeIdentifier(id value)
{
    if ([value isKindOfClass:[NSString class]]) return value;
    if ([value isKindOfClass:[NSNumber class]]) return [value stringValue];
    return nil;
}

/* The conversation a ChatKit notification is about, as the identifier the
 * request module resolves (persistentID, groupID, or uniqueIdentifier). */
static NSString *BBBridgeEventTarget(id notification)
{
    id object;
    NSString *target;
    if (![notification isKindOfClass:[NSNotification class]]) return nil;
    object = [(NSNotification *)notification object];
    target = BBBridgeIdentifier(BBBridgeObject(object, sel_registerName("persistentID")));
    if ([target length]) return target;
    target = BBBridgeIdentifier(BBBridgeObject(object, sel_registerName("groupID")));
    if ([target length]) return target;
    return BBBridgeIdentifier(BBBridgeObject(object, sel_registerName("uniqueIdentifier")));
}

static void BBBridgeHandleChatItemsDidChange(id notification)
{
    @try {
        NSDictionary *userInfo;
        id inserted;
        NSString *target;
        if (![notification isKindOfClass:[NSNotification class]]) return;
        userInfo = [(NSNotification *)notification userInfo];
        inserted = [userInfo isKindOfClass:[NSDictionary class]] ?
            [userInfo objectForKey:@"__kIMChatItemsInserted"] : nil;
        if (![inserted respondsToSelector:@selector(count)] || [inserted count] == 0) return;
        target = BBBridgeEventTarget(notification);
        if ([target length]) BBBridgeEmitUpdatedMessageForTarget(target);
    } @catch (NSException *exception) {
        (void)exception;
    }
}

static void BBBridgeHandleMessageReceived(id notification)
{
    @try {
        NSString *target = BBBridgeEventTarget(notification);
        if ([target length]) BBBridgeEmitNewMessageForTarget(target);
    } @catch (NSException *exception) {
        (void)exception;
    }
}

%hook CKConversationListController

- (void)_chatItemsDidChange:(id)notification
{
    %orig;
    BBBridgeHandleChatItemsDidChange(notification);
}

%end

%hook CKConversationList

- (void)_messageReceived:(id)notification
{
    %orig;
    BBBridgeHandleMessageReceived(notification);
}

%end

static void BBBridgeInstallIPC(void)
{
    BBBridgeCenter = [[CPDistributedMessagingCenter centerNamed:BBServerCenterName] retain];
    if (!BBBridgeCenter) {
        bb_trace(BBTraceMobileSMS, BBTraceCenterUnavailable);
        return;
    }
    rocketbootstrap_distributedmessagingcenter_apply(BBBridgeCenter);
    bb_trace(BBTraceMobileSMS, BBTraceCenterReady);
    BBBridgeInstallTypedRequests();
    bb_trace(BBTraceMobileSMS, BBTraceReceiverReady);
}

__attribute__((constructor))
static void BBBridgeInit(void)
{
    @autoreleasepool {
        bb_trace(BBTraceMobileSMS, BBTraceLoaded);
        BBBridgeInstallIPC();
    }
}
