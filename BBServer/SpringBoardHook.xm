#import <Foundation/Foundation.h>

#import "BBServerTransport.h"

static BOOL BBServerTransportStartAttempted = NO;

/* Socket setup waits for SpringBoard's launch boundary instead of running
 * from the image constructor. */
static void BBServerStartTransport(void)
{
    BOOL started;
    if (BBServerTransportStartAttempted) return;
    BBServerTransportStartAttempted = YES;
    started = [[BBServerTransport sharedTransport] start];
    NSLog(@"BBServer transport-started=%d", started);
}

%hook SpringBoard

- (void)applicationDidFinishLaunching:(id)application
{
    %orig;
    BBServerStartTransport();
}

%end
