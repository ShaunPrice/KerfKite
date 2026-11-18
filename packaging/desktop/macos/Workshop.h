#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

NS_ASSUME_NONNULL_BEGIN
NSURL * _Nullable KKWorkshopURL(NSString *address);

@interface KKWorkshop : NSWindowController <NSWindowDelegate, WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate,
    NSToolbarDelegate, NSTableViewDataSource, NSTableViewDelegate>
- (instancetype)initWithURL:(NSURL *)url;
- (void)installMenus;
- (void)confirmClosing:(void (^)(BOOL))completion;
@end
NS_ASSUME_NONNULL_END
