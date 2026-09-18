#ifndef BBBRIDGE_RUNTIME_H
#define BBBRIDGE_RUNTIME_H

#import <Foundation/Foundation.h>

@class CPDistributedMessagingCenter;
/* The SpringBoard-hosted center this bridge replies through, or nil. */
CPDistributedMessagingCenter *BBBridgeSharedCenter(void);

#endif
