#import <Foundation/Foundation.h>

#import <AppSupport/CPDistributedMessagingCenter.h>
#import <rocketbootstrap/rocketbootstrap.h>

#import "BBServerRuntime.h"
#import "BBServerTransport.h"
#import "BBTrace.h"

NSString * const BBServerCenterName = @"com.bluebubbles.bridge";

static BOOL BBServerIPCInstalled = NO;

/* SpringBoard hosts the messaging center; the bridge in MobileSMS sends
 * typed replies and events to it (docs/ARCHITECTURE.md). Messages
 * are copied and handled on the transport thread. */
@interface BBServerIPC : NSObject
- (void)runIPCServer:(id)unused;
@end

@implementation BBServerIPC

- (NSDictionary *)handleMessage:(NSString *)messageName userInfo:(NSDictionary *)userInfo
{
    if ([messageName isEqualToString:BBServerBridgeReplyMessageName]) {
        [[BBServerTransport sharedTransport] deliverBridgeReply:userInfo];
    } else if ([messageName isEqualToString:BBServerBridgeEventMessageName]) {
        [[BBServerTransport sharedTransport] deliverBridgeEvent:userInfo];
    }
    return @{ @"status": @"forwarded" };
}

- (void)runIPCServer:(id)unused
{
    (void)unused;
    @autoreleasepool {
        /* Creation, registration, and servicing all belong to this dedicated
         * thread; runServerOnCurrentThread returns immediately on iOS 9.3.5,
         * so the run loop is driven here. */
        CPDistributedMessagingCenter *center =
            [[CPDistributedMessagingCenter centerNamed:BBServerCenterName] retain];
        if (!center) {
            bb_trace(BBTraceSpringBoard, BBTraceCenterUnavailable);
            return;
        }
        rocketbootstrap_distributedmessagingcenter_apply(center);
        bb_trace(BBTraceSpringBoard, BBTraceCenterReady);
        [center registerForMessageName:BBServerBridgeReplyMessageName
                                 target:self
                               selector:@selector(handleMessage:userInfo:)];
        [center registerForMessageName:BBServerBridgeEventMessageName
                                 target:self
                               selector:@selector(handleMessage:userInfo:)];
        bb_trace(BBTraceSpringBoard, BBTraceReceiverReady);
        [center runServerOnCurrentThread];

        NSRunLoop *loop = [NSRunLoop currentRunLoop];
        BOOL hasSources = YES;
        while (hasSources) {
            @autoreleasepool {
                hasSources = [loop runMode:NSDefaultRunLoopMode
                               beforeDate:[NSDate dateWithTimeIntervalSinceNow:30.0]];
            }
        }
        [center stopServer];
        [center release];
    }
}

@end

static void BBServerInstallIPC(void)
{
    BBServerIPC *handler;
    if (BBServerIPCInstalled) return;
    handler = [[BBServerIPC alloc] init];
    if (!handler) return;
    BBServerIPCInstalled = YES;
    /* NSThread retains its target for the lifetime of the detached thread. */
    [NSThread detachNewThreadSelector:@selector(runIPCServer:) toTarget:handler withObject:nil];
    [handler release];
}

__attribute__((constructor))
static void BBServerInit(void)
{
    @autoreleasepool {
        bb_trace(BBTraceSpringBoard, BBTraceLoaded);
        BBServerInstallIPC();
    }
}
