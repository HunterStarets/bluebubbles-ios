#ifndef BB_BRIDGE_REQUESTS_H
#define BB_BRIDGE_REQUESTS_H

#import <Foundation/Foundation.h>

/* MobileSMS side of the typed IPC contract (docs/ARCHITECTURE.md): every
 * operation answered with BlueBubbles-shaped JSON, and new/updated message
 * events. Everything runs on the main queue. */

/* Observe the typed request notification. Called once from the bridge's
 * IPC installation after the center exists. */
void BBBridgeInstallTypedRequests(void);

/* Event hooks, called from the CKConversationList observers with the
 * conversation's identifier (persistentID/groupID/uniqueIdentifier). */
void BBBridgeEmitNewMessageForTarget(NSString *target);
void BBBridgeEmitUpdatedMessageForTarget(NSString *target);

#endif
