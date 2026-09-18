#import <UIKit/UIKit.h>

/* The two Preferences.framework classes this pane uses, declared here
 * because the iPhoneOS 9.3 SDK cannot compile the vendored headers. The
 * framework itself provides the implementations at load time. */
@interface PSSpecifier : NSObject
@end

@interface PSListController : UIViewController {
    NSArray *_specifiers;
}
- (NSArray *)loadSpecifiersFromPlistName:(NSString *)name target:(id)target;
- (void)reloadSpecifiers;
@end

@interface BBSRootListController : PSListController
- (id)serverAddressForSpecifier:(PSSpecifier *)specifier;
- (id)passwordForSpecifier:(PSSpecifier *)specifier;
- (void)setPassword:(id)value forSpecifier:(PSSpecifier *)specifier;
- (id)versionForSpecifier:(PSSpecifier *)specifier;
- (void)openGitHub:(PSSpecifier *)specifier;
@end
