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
