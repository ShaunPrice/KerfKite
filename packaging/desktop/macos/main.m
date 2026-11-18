#import "Workshop.h"

@interface KKApplication : NSObject <NSApplicationDelegate>
@property (strong) NSURL *url;
@property (strong) KKWorkshop *workshop;
@end
@implementation KKApplication
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    self.workshop = [[KKWorkshop alloc] initWithURL:self.url];
    [self.workshop installMenus];
    [self.workshop showWindow:self];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    if (!self.workshop.window.isVisible) return NSTerminateNow;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.workshop confirmClosing:^(BOOL discard) { [app replyToApplicationShouldTerminate:discard]; }];
    });
    return NSTerminateLater;
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSURL *url = argc == 2 ? KKWorkshopURL([NSString stringWithUTF8String:argv[1]]) : nil;
        if (!url) { fprintf(stderr, "Supply the local KerfKite workstation address.\n"); return 2; }
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        KKApplication *delegate = [KKApplication new];
        delegate.url = url;
        app.delegate = delegate;
        [app run];
        (void)delegate.workshop;
    }
    return 0;
}
