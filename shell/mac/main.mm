#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <exception>
#include <memory>
#include <string>

#include "devcard/server.h"

namespace {

void show_error(const std::string& message) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"DevCard 启动失败";
  alert.informativeText = [NSString stringWithUTF8String:message.c_str()];
  [alert addButtonWithTitle:@"退出"];
  [alert runModal];
}

}  // namespace

// WKWebView 默认不处理 <a download> / Content-Disposition 下载，导出会静默失败。
// 这里把下载接到系统「下载」目录，中文文件名原样保留。
API_AVAILABLE(macos(11.0))
@interface DownloadHandler : NSObject <WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate>
@end

@implementation DownloadHandler

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)navigationAction
                    preferences:(WKWebpagePreferences*)preferences
            decisionHandler:(void (^)(WKNavigationActionPolicy,
                                      WKWebpagePreferences*))decisionHandler {
  if (navigationAction.shouldPerformDownload) {
    decisionHandler(WKNavigationActionPolicyDownload, preferences);
    return;
  }
  decisionHandler(WKNavigationActionPolicyAllow, preferences);
}

- (void)webView:(WKWebView*)webView
    navigationAction:(WKNavigationAction*)navigationAction
    didBecomeDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
  download.delegate = self;
}

- (void)webView:(WKWebView*)webView
    navigationResponse:(WKNavigationResponse*)navigationResponse
     didBecomeDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
  download.delegate = self;
}

- (void)download:(WKDownload*)download
    decideDestinationUsingResponse:(NSURLResponse*)response
                 suggestedFilename:(NSString*)suggestedFilename
              completionHandler:(void (^)(NSURL* _Nullable))completionHandler
    API_AVAILABLE(macos(11.3)) {
  NSString* name = suggestedFilename.length > 0 ? suggestedFilename : @"download.xlsx";
  NSURL* directory = [[NSFileManager defaultManager]
      URLForDirectory:NSDownloadsDirectory
             inDomain:NSUserDomainMask
    appropriateForURL:nil
               create:YES
                error:nil];
  completionHandler([directory URLByAppendingPathComponent:name]);
}

@end

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    std::unique_ptr<devcard::ServerHandle> server;
    try {
      devcard::ServerOptions options;
      options.data_dir = devcard::default_data_dir();
      options.port = 18080;
      server = devcard::start_server(options);
    } catch (const std::exception& error) {
      show_error(error.what());
      return 1;
    }

    NSMenu* menu = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [menu addItem:app_item];
    NSMenu* app_menu = [[NSMenu alloc] init];
    [app_menu addItemWithTitle:@"退出 DevCard"
                        action:@selector(terminate:)
                 keyEquivalent:@"q"];
    app_item.submenu = app_menu;
    NSApp.mainMenu = menu;

    NSRect frame = NSMakeRect(0, 0, 1080, 720);
    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:(NSWindowStyleMaskTitled |
                                               NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskMiniaturizable |
                                               NSWindowStyleMaskResizable)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.title = @"设备管理卡";
    [window center];

    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    WKWebView* web = [[WKWebView alloc] initWithFrame:frame configuration:config];
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    DownloadHandler* download_handler = [[DownloadHandler alloc] init];
    web.navigationDelegate = download_handler;
    web.UIDelegate = download_handler;
    window.contentView = web;

    NSString* url_string =
        [NSString stringWithFormat:@"http://127.0.0.1:%d/", server->port()];
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url_string]]];

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }
  return 0;
}
