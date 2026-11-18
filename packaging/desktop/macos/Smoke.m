/* Runs in a separate process against Studio's simulator on a GUI-capable Mac. */
#import "Workshop.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>

@interface KKWorkshop (Smoke)
- (void)invoke:(NSString *)action payload:(NSDictionary *)p reply:(void (^)(id,NSString *))reply;
- (void)openDocument:(NSURL *)url;
- (void)writeContent:(NSString *)content toURL:(NSURL *)url project:(BOOL)project;
@end

@interface KKSmokeWindow : KKWorkshop
@property (strong) NSURL *externalLink;
@end
@implementation KKSmokeWindow
- (void)openLink:(NSURL *)url { self.externalLink = url; }

@end

static void Check(BOOL ok, NSString *why) {
    if (!ok) { fprintf(stderr,"FAIL: %s\n",why.UTF8String); exit(1); }
}
static BOOL Wait(NSTimeInterval seconds, BOOL (^predicate)(void)) {
    NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!predicate() && limit.timeIntervalSinceNow > 0) {
        NSDate *tick = [NSDate dateWithTimeIntervalSinceNow:.035];
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:tick inMode:NSDefaultRunLoopMode dequeue:YES];
        if (event) [NSApp sendEvent:event];
        [NSApp updateWindows];
    }
    return predicate();
}
static id Invoke(KKWorkshop *host,NSString *action,NSDictionary *p) {
    __block BOOL done = NO; __block id out = nil; __block NSString *reason = nil;
    [host invoke:action payload:p reply:^(id value,NSString *error){ out = value; reason = error; done = YES; }];
    Check(Wait(30, ^BOOL{ return done; }),action); Check(!reason,[NSString stringWithFormat:@"%@ %@ failed: %@",action,p,reason]); return out;
}
static NSButton *FindButton(NSView *v, NSString *name) {
    if ([v isKindOfClass:NSButton.class] && [[(NSButton *)v title] isEqual:name]) return (NSButton *)v;
    for (NSView *child in v.subviews) { NSButton *b = FindButton(child,name); if (b) return b; }
    return nil;
}
static void Click(KKWorkshop *host, NSString *name) {
    NSButton *b = FindButton(host.window.contentView,name); Check(b && b.enabled,name); [b performClick:nil];
    Check(Wait(20,^BOOL{ return ![[host valueForKey:@"busy"] boolValue]; }),name);
}
static void JS(WKWebView *web, NSString *script) {
    __block BOOL done = NO; __block NSError *failure = nil;
    [web evaluateJavaScript:script completionHandler:^(id value,NSError *error){ failure = error; done = YES; }];
    Check(Wait(20,^BOOL{ return done; }),@"page action timeout"); Check(!failure,failure.localizedDescription);
}
static void ActivateMenu(KKWorkshop *host, NSString *menu, NSString *item) {
    NSMenu *submenu = [NSApp.mainMenu itemWithTitle:menu].submenu;
    NSInteger index = [submenu indexOfItemWithTitle:item];
    Check(submenu && index >= 0,item);
    [submenu performActionForItemAtIndex:index];
    Check(Wait(5,^BOOL{ return ![[host valueForKey:@"busy"] boolValue]; }),item);
}
static BOOL CaptureOwnWindow(NSWindow *window,NSString *path) {
    if (@available(macOS 14.4,*)) {
        __block BOOL done = NO; __block NSData *png = nil;
        CGWindowID windowID = (CGWindowID)window.windowNumber;
        [SCShareableContent getCurrentProcessShareableContentWithCompletionHandler:^(SCShareableContent *content,NSError *error){
            for (SCWindow *w in content.windows) {
                if (w.windowID != windowID) continue;
                SCContentFilter *filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:w];
                SCStreamConfiguration *config = [SCStreamConfiguration new];
                config.width = (size_t)w.frame.size.width; config.height = (size_t)w.frame.size.height;
                config.showsCursor = NO; config.ignoreShadowsSingleWindow = YES;
                [SCScreenshotManager captureImageWithFilter:filter configuration:config completionHandler:^(CGImageRef image,NSError *failure){
                    NSData *capture = image ? [[[NSBitmapImageRep alloc] initWithCGImage:image] representationUsingType:NSBitmapImageFileTypePNG properties:@{}] : nil;
                    dispatch_async(dispatch_get_main_queue(), ^{ png = capture; done = YES; });
                }]; return;
            }
            dispatch_async(dispatch_get_main_queue(), ^{ done = YES; });
        }];
        if (Wait(10,^BOOL{ return done; }) && png) return [png writeToFile:path atomically:YES];
    }
    return NO;
}
static void Run(KKWorkshop *host,NSURL *url,NSString *imagePath) {
    Check(Wait(35,^BOOL{ return [[host valueForKey:@"ready"] boolValue]; }),@"editor did not start");
    Invoke(host,@"new",nil);
    Check(Wait(3,^BOOL{ return [(NSArray *)[host valueForKey:@"objects"] count] == 0; }),@"empty object list");
    NSArray<NSTextField *> *fields = [host valueForKey:@"fields"];
    NSPopUpButton *kind = [host valueForKey:@"shapeType"];
    fields[0].stringValue = @"40"; fields[1].stringValue = @"50"; fields[2].stringValue = @"200"; fields[3].stringValue = @"115";
    Click(host,@"Add shape");
    NSDictionary *state = Invoke(host,@"state",nil); Check([state[@"objects"] count] == 1 && [state[@"dirty"] boolValue],@"native rectangle");
    fields[0].stringValue = @"217"; fields[1].stringValue = @"95"; fields[2].stringValue = @"65"; fields[3].stringValue = @"65"; [kind selectItemWithTitle:@"Ellipse"];
    Click(host,@"Add shape"); Invoke(host,@"state",nil);
    NSTableView *table = [host valueForKey:@"table"];
    Check(Wait(3,^BOOL{ return table.numberOfRows == 2; }),@"native table update");
    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    Check(Wait(5,^BOOL{ return [[host valueForKey:@"lastIndex"] integerValue] == 0; }),@"native selection");
    fields[0].stringValue = @"55"; fields[1].stringValue = @"60"; Click(host,@"Move selected object");
    state = Invoke(host,@"state",nil); Check([state[@"selection"][@"x"] intValue] == 55,@"native geometry edit");
    Invoke(host,@"undo",nil); Invoke(host,@"select",@{@"index":@0}); Check([Invoke(host,@"state",nil)[@"selection"][@"x"] intValue] == 40,@"undo movement");
    Invoke(host,@"redo",nil);
    // Use the editor's stock art workflow to exercise a richer engraving.
    WKWebView *web = [host valueForKey:@"web"];
    [host.window makeFirstResponder:fields[0]];
    ActivateMenu(host,@"Workspace",@"CAD Command…");
    JS(web,@"if (document.activeElement.id !== 'cad-command') throw Error('Command focus'); document.execCommand('insertText',false,'view top');");
    ActivateMenu(host,@"Edit",@"Undo");
    JS(web,@"if (document.activeElement.value !== '') throw Error('Command undo');");
    ActivateMenu(host,@"Edit",@"Redo");
    JS(web,@"if (document.activeElement.value !== 'view top') throw Error('Command redo');");
    ActivateMenu(host,@"Workspace",@"CAD Script…");
    JS(web,@"if (!document.getElementById('cad-script-dialog').open || document.activeElement.id !== 'cad-script-source') throw Error('Script focus'); document.getElementById('cad-script-dialog').close();");
    Check([Invoke(host,@"state",nil)[@"objects"] count] == 2,@"CAD authoring preserved the drawing");
    JS(web,@"(()=>{const t=CNCTemplates.templates.find(t=>/rosette/i.test(t.name)); if(!t) throw Error('missing rosette'); addGenerated(t.name,t.paths.map(p=>p.map(([x,y])=>[90+x,65+y])));})()");
    Invoke(host,@"preview",nil);
    NSDictionary *code = Invoke(host,@"export",nil); Check([code[@"content"] containsString:@"G1"],@"G-code after preview");
    NSDictionary *project = Invoke(host,@"save",nil);
    NSURL *tmp = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingString:@".cncstudio.json"]]];
    [host writeContent:project[@"content"] toURL:tmp project:YES];
    Check(Wait(6,^BOOL{ return ![[host valueForKey:@"dirty"] boolValue]; }),@"native save");
    Invoke(host,@"new",nil);
    Check(Wait(5,^BOOL{ return [(NSArray *)[host valueForKey:@"objects"] count] == 0; }),@"blank before open");
    [host openDocument:tmp];
    Check(Wait(15,^BOOL{ return [(NSArray *)[host valueForKey:@"objects"] count] == 3; }),@"native project round trip");
    [NSFileManager.defaultManager removeItemAtURL:tmp error:nil];
    Invoke(host,@"select",@{@"index":@2}); Invoke(host,@"move",@{@"x":@105,@"y":@70});
    __block BOOL answered = NO, discard = YES;
    [host confirmClosing:^(BOOL allow){ discard = allow; answered = YES; }];
    Check(Wait(5,^BOOL{ return host.window.attachedSheet != nil; }),@"dirty close sheet");
    [host.window endSheet:host.window.attachedSheet returnCode:NSAlertFirstButtonReturn];
    Check(Wait(5,^BOOL{ return answered; }) && !discard,@"cancel protects artwork");
    Check(!KKWorkshopURL(@"http://user@127.0.0.1:18765/") && !KKWorkshopURL(@"http://127.0.0.1:18765/other"),@"URL validation");
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"about:blank"]]];
    Wait(.2,^BOOL{ return NO; }); Check([web.URL isEqual:url],@"reject external document");
    JS(web,@"Array.from(document.links).find(a=>a.pathname==='/help.html').click(); void 0");
    Check(Wait(4,^BOOL{ return [(KKSmokeWindow *)host externalLink] != nil; }),@"guide link routed to system browser");
    Check([[[(KKSmokeWindow *)host externalLink] path] isEqual:@"/help.html"],@"help destination");
    JS(web,@"$('printDesign').click(); void 0");
    Check(Wait(4,^BOOL{ return [host valueForKey:@"printWindow"] != nil; }),@"print preview opened");
    NSWindowController *printView = [host valueForKey:@"printWindow"];
    WKWebView *page = [printView valueForKey:@"page"];
    JS(page,@"if(!document.querySelector('svg polyline')) throw Error('missing design print geometry');");
    [printView close];
    JS(web,@"$('printQuote').click(); void 0");
    Check(Wait(5,^BOOL{ return [host valueForKey:@"printWindow"] != printView; }),@"quote print preview opened");
    printView = [host valueForKey:@"printWindow"];
    JS([printView valueForKey:@"page"],@"if(!document.querySelector('h1')?.textContent.includes('Manufacturing quote')) throw Error('missing printed quote');");
    [printView close];
    if (imagePath) {
        JS(web,@"$('projectName').value='Rosette plaque'; $('projectName').dispatchEvent(new Event('input',{bubbles:true})); void 0");
        Invoke(host,@"preview",nil);

        [host.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; Wait(.6,^BOOL{ return NO; });
        if (!CaptureOwnWindow(host.window,imagePath)) {
        // Capture only this application's views. Accelerated WKWebView content uses its own snapshot API.
        // WindowServer composited toolbar effects cannot be captured from a view bitmap.
        NSView *frame = host.window.contentView;
        NSBitmapImageRep *chrome = [frame bitmapImageRepForCachingDisplayInRect:frame.bounds];
        [frame cacheDisplayInRect:frame.bounds toBitmapImageRep:chrome];
        __block NSImage *viewport = nil; __block BOOL finished = NO;
        [web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *view,NSError *error){ viewport = view; finished = YES; }];
        Check(Wait(12,^BOOL{ return finished; }) && viewport,@"WKWebView snapshot");
        NSRect webRect = [web convertRect:web.bounds toView:frame];
        NSImage *picture = [NSImage imageWithSize:frame.bounds.size flipped:frame.isFlipped drawingHandler:^BOOL(NSRect bounds){
            [chrome drawInRect:bounds]; [viewport drawInRect:webRect]; return YES;
        }];
        NSBitmapImageRep *bitmap = [NSBitmapImageRep imageRepWithData:picture.TIFFRepresentation];
        Check([[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:imagePath atomically:YES],@"native window PNG");
        }

    }
    printf("PASS: AppKit window, native creation, movement, selection, undo/redo, WKWebView preview, G-code, project round trip, discard cancellation, navigation confinement, help link, design and quote print\n");
    exit(0);
}
int main(int argc,const char *argv[]) {
    @autoreleasepool {
        if (argc < 2 || argc > 3) return 2;
        NSURL *url = KKWorkshopURL([NSString stringWithUTF8String:argv[1]]); if (!url) return 2;
        NSString *path = argc == 3 ? [NSString stringWithUTF8String:argv[2]] : nil;
        NSApplication *app = NSApplication.sharedApplication; [app setActivationPolicy:NSApplicationActivationPolicyRegular]; [app finishLaunching];
        KKWorkshop *host = [[KKSmokeWindow alloc] initWithURL:url]; [host installMenus]; [host showWindow:nil]; [app activateIgnoringOtherApps:YES];
        [NSTimer scheduledTimerWithTimeInterval:.1 repeats:NO block:^(NSTimer *timer){ Run(host,url,path); }];
        [app run];
    }
    return 0;
}
