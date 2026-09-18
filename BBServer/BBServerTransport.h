#ifndef BB_SERVER_TRANSPORT_H
#define BB_SERVER_TRANSPORT_H

#import <Foundation/Foundation.h>

/* Distributed notification carrying one typed bridge request from the
 * SpringBoard server to the MobileSMS bridge. userInfo keys: requestID
 * (decimal string), operation, argumentsJSON, replyKind. See
 * docs/ARCHITECTURE.md. */
FOUNDATION_EXPORT NSString * const BBServerBridgeRequestNotificationName;

/* Center message names the MobileSMS bridge sends back. */
FOUNDATION_EXPORT NSString * const BBServerBridgeReplyMessageName;
FOUNDATION_EXPORT NSString * const BBServerBridgeEventMessageName;

/* The BlueBubbles HTTP(S) + Socket.IO listener. All protocol logic lives in
 * the host-tested BB* C modules; this object owns sockets, buffers, the
 * clock, and the hand-off to the MobileSMS bridge. Everything except the
 * two deliver methods runs on the transport's own thread. */
@interface BBServerTransport : NSObject

+ (instancetype)sharedTransport;

/* Start the transport thread. Returns NO when settings cannot be read;
 * nothing listens in that case. The listener itself opens only while the
 * "enabled" setting is on and follows setting changes live. */
- (BOOL)start;
- (BOOL)isReady;

/* Called from the IPC thread with the bridge's reply/event dictionary. The
 * dictionary is copied and handled on the transport thread. */
- (void)deliverBridgeReply:(NSDictionary *)userInfo;
- (void)deliverBridgeEvent:(NSDictionary *)userInfo;

@end

#endif
