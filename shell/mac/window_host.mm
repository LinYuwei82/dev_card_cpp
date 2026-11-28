// macOS 壳的窗口/WebView 宿主（M4 从 main.mm 拆出）。
#import "window_host.h"

#import <WebKit/WebKit.h>

#import "about_panel.h"
#import "app_delegate.h"
#import "bridge.h"
#import "download_handler.h"
#import "print_export.h"
#import "print_selftest.h"
#import "shell_state.h"
#import "updater.h"

@interface DevCardWindowHost ()
@property(nonatomic, retain) NSWindow* window;
@property(nonatomic, retain) WKWebView* webView;
// server 就绪后要导航到的路径（打印自检时为 /?view=print）。
@property(nonatomic, retain) NSString* page;
@end

@implementation DevCardWindowHost

- (void)present {
  NSMenu* menu = [[NSMenu alloc] init];
  NSMenuItem* app_item = [[NSMenuItem alloc] init];
  [menu addItem:app_item];
  NSMenu* app_menu = [[NSMenu alloc] init];

  // 关于 DevCard：系统惯例放应用菜单首位。target 用常驻实例，随进程存活。
  DevCardAboutHelper* about_handler = DevCardSharedAboutHandler();
  NSMenuItem* about_item =
      [app_menu addItemWithTitle:@"关于 DevCard"
                           action:@selector(showAbout:)
                    keyEquivalent:@""];
  about_item.target = about_handler;
  [app_menu addItem:[NSMenuItem separatorItem]];

  // 原生更新引擎（core UpdateEngine + NSAlert + Sparkle 安装执行器）：
  // target/action 与可见性由 bridge.start 按清单配置情况绑定。
  NSMenuItem* check_update_item =
      [app_menu addItemWithTitle:@"检查更新…"
                           action:nil
                    keyEquivalent:@""];
  check_update_item.target = nil;  // 稍后由 DevCardUpdaterBridge 接管

  [app_menu addItem:[NSMenuItem separatorItem]];
  [app_menu addItemWithTitle:@"退出 DevCard"
                      action:@selector(terminate:)
               keyEquivalent:@"q"];
  app_item.submenu = app_menu;
  NSApp.mainMenu = menu;

  NSRect frame = NSMakeRect(0, 0, 1280, 840);
  NSWindow* window =
      [[NSWindow alloc] initWithContentRect:frame
                                  styleMask:(NSWindowStyleMaskTitled |
                                             NSWindowStyleMaskClosable |
                                             NSWindowStyleMaskMiniaturizable |
                                             NSWindowStyleMaskResizable)
                                    backing:NSBackingStoreBuffered
                                      defer:NO];
  window.title = @"设备管理卡";
  // 与小兔进销存同口径：窗口外框最小 960×600（不用 contentMinSize），
  // 保证 980 断点可触发、又不至于窄到内容挤成一列。
  [window setMinSize:NSMakeSize(960, 600)];
  [window center];
  self.window = window;

  WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
  WKUserContentController* controller = [[WKUserContentController alloc] init];
  DevCardBridge* bridge = [[DevCardBridge alloc] init];
  bridge.updaterBridge = [[DevCardUpdaterBridge alloc] init];
  bridge.updaterBridge.checkUpdateMenuItem = check_update_item;
  // 状态栏版本号点击（showAbout）复用同一个关于处理器。
  bridge.aboutHandler = about_handler;
  [controller addScriptMessageHandler:bridge name:@"devcard"];
  // 页面据此判断是壳内还是普通浏览器，决定「导出 PDF」按钮的行为。
  [controller
      addUserScript:[[WKUserScript alloc]
                        initWithSource:@"window.__devcardShell = true;"
                         injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                      forMainFrameOnly:YES]];
  config.userContentController = controller;

  WKWebView* web = [[WKWebView alloc] initWithFrame:frame configuration:config];
  web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  self.webView = web;

  DevCardDownloadHandler* download_handler =
      [[DevCardDownloadHandler alloc] init];
  download_handler.window = window;
  // 打印自检启动钩子：必须等真实页（http://）didFinish 之后再发起 JS——
  // splash→真实页导航进行中时 evaluateJavaScript 会被 WebKit 竞态吞掉。
  download_handler.onDidFinish = ^{
    DevCardStartupLog("webview_did_finish");
    DevCardSelftestKickoff(web);
  };
  web.navigationDelegate = download_handler;
  web.UIDelegate = download_handler;
  window.contentView = web;

  bridge.webView = web;
  bridge.window = window;
  // 导出/打印通道独立持有 webView + window，桥只转发 action。
  DevCardPrintBridge* print_bridge = [[DevCardPrintBridge alloc] init];
  print_bridge.webView = web;
  print_bridge.window = window;
  bridge.printBridge = print_bridge;

  // 关键：窗口/应用生命周期代理单独持有，否则红叉关闭窗口后不会退出应用。
  DevCardAppDelegate* app_delegate = [[DevCardAppDelegate alloc] init];
  window.delegate = app_delegate;
  [NSApp setDelegate:app_delegate];

  // Sparkle 2 自更新：从 Info.plist 读 SUFeedURL + SUPublicEDKey 初始化，
  // 同时绑定 App 菜单 target + 处理 DEVCARD_SPARKLE_SELFTEST。
  [bridge.updaterBridge start];

  // 自动化验证：把打印视图导出成 PDF 后退出，便于脚本核对页数与文字。
  print_bridge.automaticPdfPath = DevCardSelftestPdfPath();
  self.page = DevCardSelftestPagePath();
  DevCardSelftestPrepare(web, print_bridge);

  // splash 用内联 HTML，不依赖 server：窗口先出现，server 在后台线程起。
  [web loadHTMLString:
           @"<!doctype html><html><head><meta charset='utf-8'>"
            "<style>html,body{height:100%;margin:0;"
            "font-family:-apple-system,'PingFang SC',sans-serif;"
            "display:flex;align-items:center;justify-content:center;"
            "background:#f5f6f8;color:#666}"
            ".spin{width:28px;height:28px;border:3px solid #d0d5dd;"
            "border-top-color:#2563eb;border-radius:50%;"
            "animation:s 1s linear infinite}"
            "@keyframes s{to{transform:rotate(360deg)}}"
            "p{margin-top:14px;font-size:14px}</style></head>"
            "<body><div><div class='spin' style='margin:0 auto'></div>"
            "<p style='text-align:center'>正在启动 DevCard…</p>"
            "</div></body></html>"
       baseURL:nil];
  [window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  DevCardStartupLog("shell_window_visible");
}

- (void)navigateToPageWithPort:(int)port {
  NSString* url_string =
      [NSString stringWithFormat:@"http://127.0.0.1:%d%@", port, self.page];
  [self.webView loadRequest:[NSURLRequest
                               requestWithURL:[NSURL URLWithString:url_string]]];
}

@end
