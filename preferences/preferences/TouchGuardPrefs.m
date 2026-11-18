// CallRevive settings. Runtime identifiers retained for upgrade compatibility.
#import <Preferences/PSListController.h>
#import <Preferences/PSSpecifier.h>
#import <UIKit/UIKit.h>

@interface TouchGuardPrefsListController : PSListController
- (void)openGitHub;
@end

@implementation TouchGuardPrefsListController
- (NSArray *)specifiers {
    if (!_specifiers) {
        _specifiers = [self loadSpecifiersFromPlistName:@"Root" target:self];
    }
    return _specifiers;
}
- (void)applySettings {
    CFNotificationCenterPostNotification(
        CFNotificationCenterGetDarwinNotifyCenter(),
        CFSTR("com.mohsen.touchguard.settings-changed"),
        NULL, NULL, true);
}
- (void)openGitHub {
    NSURL *url = [NSURL URLWithString:@"https://github.com/mahasenovic/CallRevive"];
    if (url) [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
}
@end
