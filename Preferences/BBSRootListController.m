#import <Foundation/Foundation.h>
#import <arpa/inet.h>
#import <ifaddrs.h>
#import <netinet/in.h>
#import <net/if.h>
#import "BBSRootListController.h"

/* The pane edits the preferences domain the server reads and posts the
 * Darwin notification the server reloads on (PostNotification in
 * Root.plist). The one computed value is the address to type into the
 * client: scheme from the HTTPS setting, the device's Wi-Fi IPv4, the port. */

#define BBS_DOMAIN CFSTR("com.hunterstarets.bluebubbles-ios")

static NSString *BBSWiFiAddress(void)
{
    struct ifaddrs *interfaces = NULL;
    NSString *preferred = nil;
    NSString *fallback = nil;
    if (getifaddrs(&interfaces) != 0) return nil;
    for (struct ifaddrs *cursor = interfaces; cursor; cursor = cursor->ifa_next) {
        char text[INET_ADDRSTRLEN];
        NSString *name;
        if (!cursor->ifa_addr || cursor->ifa_addr->sa_family != AF_INET) continue;
        if (!(cursor->ifa_flags & IFF_UP) || (cursor->ifa_flags & IFF_LOOPBACK)) continue;
        if (!inet_ntop(AF_INET, &((struct sockaddr_in *)cursor->ifa_addr)->sin_addr, text, sizeof(text)))
            continue;
        name = [NSString stringWithUTF8String:cursor->ifa_name ?: ""];
        if ([name isEqualToString:@"en0"] && !preferred) preferred = [NSString stringWithUTF8String:text];
        else if (!fallback) fallback = [NSString stringWithUTF8String:text];
    }
    freeifaddrs(interfaces);
    return preferred ?: fallback;
}

@implementation BBSRootListController

- (NSArray *)specifiers
{
    if (!_specifiers) {
        _specifiers = [[self loadSpecifiersFromPlistName:@"Root" target:self] retain];
    }
    return _specifiers;
}

- (void)viewWillAppear:(BOOL)animated
{
    [super viewWillAppear:animated];
    /* The address depends on settings edited on this same page. */
    [self reloadSpecifiers];
}

- (id)serverAddressForSpecifier:(PSSpecifier *)specifier
{
    id tls = [(id)CFPreferencesCopyAppValue(CFSTR("tls"), BBS_DOMAIN) autorelease];
    id port = [(id)CFPreferencesCopyAppValue(CFSTR("port"), BBS_DOMAIN) autorelease];
    id path = [(id)CFPreferencesCopyAppValue(CFSTR("certificatePath"), BBS_DOMAIN) autorelease];
    NSString *address = BBSWiFiAddress();
    int portNumber = [port respondsToSelector:@selector(intValue)] ? [port intValue] : 0;
    BOOL https = [tls respondsToSelector:@selector(boolValue)] && [tls boolValue];
    NSString *note = @"";
    (void)specifier;
    if (portNumber <= 0 || portNumber > 65535) portNumber = 1234;
    if (![address length]) return @"Not on Wi-Fi";
    if (https) {
        /* The server falls back to HTTP when the identity cannot load;
         * the visible check is whether the file is there at all. */
        NSString *certificate = [path isKindOfClass:[NSString class]] && [path length] ?
            path : @"/var/mobile/Library/BlueBubbles/server.p12";
        if (![[NSFileManager defaultManager] fileExistsAtPath:certificate]) {
            https = NO;
            note = @" (no certificate)";
        }
    }
    return [NSString stringWithFormat:@"%@://%@:%d%@", https ? @"https" : @"http",
            address, portNumber, note];
}

- (id)passwordForSpecifier:(PSSpecifier *)specifier
{
    /* The server generates a password on first launch; show it. */
    id value = [(id)CFPreferencesCopyAppValue(CFSTR("password"), BBS_DOMAIN) autorelease];
    (void)specifier;
    return [value isKindOfClass:[NSString class]] ? value : @"";
}

- (void)setPassword:(id)value forSpecifier:(PSSpecifier *)specifier
{
    NSString *password = [value isKindOfClass:[NSString class]] ? value : @"";
    (void)specifier;
    if (![password length]) return;   /* never store an empty password */
    CFPreferencesSetAppValue(CFSTR("password"), (CFPropertyListRef)password, BBS_DOMAIN);
    CFPreferencesAppSynchronize(BBS_DOMAIN);
    CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                         CFSTR("com.hunterstarets.bluebubbles-ios/settings"),
                                         NULL, NULL, YES);
}

- (id)versionForSpecifier:(PSSpecifier *)specifier
{
    NSString *version = [[[NSBundle bundleForClass:[self class]] infoDictionary]
                         objectForKey:@"CFBundleShortVersionString"];
    (void)specifier;
    return [version length] ? version : @"";
}

- (void)openGitHub:(PSSpecifier *)specifier
{
    (void)specifier;
    [[UIApplication sharedApplication] openURL:[NSURL URLWithString:@"https://github.com/HunterStarets"]];
}

@end
