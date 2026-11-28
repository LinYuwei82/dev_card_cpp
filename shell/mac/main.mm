#import <Cocoa/Cocoa.h>
#import <PDFKit/PDFKit.h>
#import <WebKit/WebKit.h>

#import "updater.h"

#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <chrono>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "devcard/paths.h"
#include "devcard/server.h"
#include "devcard/update_manifest.h"

#import "about_panel.h"
#import "app_delegate.h"
#import "bridge.h"
#import "download_handler.h"
#import "print_export.h"
#import "shell_state.h"





int main(int argc, const char* argv[]) {
  DevCardStartupClockBegin();
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    devcard::ServerOptions options;
    options.data_dir = devcard::default_data_dir();
    options.port = 18080;

    const int instance_lock = DevCardAcquireSingleInstanceLock(options.data_dir);
    if (instance_lock == -2) {
      // 已有实例：把它叫到前台，本进程直接退出，不弹错误框。
      DevCardActivateExistingInstance();
      return 0;
    }
    if (instance_lock < 0) {
      // 锁文件打不开（目录权限等）：壳层单实例防护失效，继续启动并依赖
      // server 层的实例锁兜底，但必须留下日志，不能让失效无声无息。
      NSLog(@"[devcard] 警告：无法创建 shell.lock，壳层单实例防护跳过（errno=%d）",
            errno);
    }

    // 启动速度优化：server 初始化（含字体校验/DB 迁移，mac ~90ms、win ~250ms）
    // 移到后台线程；主线程立刻建窗口并显示内联 splash，就绪后再导航到真实页面。
    // 句柄用 shared_ptr 套壳：后台线程产出、主线程块里接管，生命周期仍随 main。
    auto server_holder =
        std::make_shared<std::unique_ptr<devcard::ServerHandle>>();

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
    // 菜单「检查更新…」的占位，稍后 bridge 创建好后存入 bridge.checkUpdateMenuItem。
    NSMenuItem* check_update_item_placeholder = check_update_item;

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

    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    WKUserContentController* controller = [[WKUserContentController alloc] init];
    DevCardBridge* bridge = [[DevCardBridge alloc] init];
    bridge.updaterBridge = [[DevCardUpdaterBridge alloc] init];
    bridge.updaterBridge.checkUpdateMenuItem = check_update_item_placeholder;
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
    DevCardDownloadHandler* download_handler =
        [[DevCardDownloadHandler alloc] init];
    download_handler.window = window;
    // 打印自检启动钩子：必须等真实页（http://）didFinish 之后再发起 JS——
    // splash→真实页导航进行中时 evaluateJavaScript 会被 WebKit 竞态吞掉。
    // 轮询函数用 std::function + shared_ptr 自引用：ObjC 块递归 + 在 GCD
    // 上下文拷贝块捕获的写法实测触发 _Block_copy(NULL) 段错误（DevCard-*.ips）。
    auto print_poll_slot = std::make_shared<std::function<void(int)>>();
    __block BOOL print_poll_started = NO;
    download_handler.onDidFinish = ^{
      DevCardStartupLog("webview_did_finish");
      if (*print_poll_slot && !print_poll_started &&
          [web.URL.absoluteString hasPrefix:@"http://"]) {
        print_poll_started = YES;
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
              (*print_poll_slot)(50);
            });
      }
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
    // 全部逻辑抽到 shell/mac/updater.mm 的 DevCardUpdaterBridge。
    [bridge.updaterBridge start];

    // 自动化验证：把打印视图导出成 PDF 后退出，便于脚本核对页数与文字。
    NSString* selftest_pdf =
        NSProcessInfo.processInfo.environment[@"DEVCARD_PRINT_SELFTEST_PDF"];
    print_bridge.automaticPdfPath = selftest_pdf;
    // 负向自检：摘掉 crypto.subtle，验证「判据没跑起来」会被单独拦住。
    BOOL selftest_nosubtle =
        NSProcessInfo.processInfo.environment[@"DEVCARD_SELFTEST_NOSUBTLE"].length > 0;

    NSString* page = selftest_pdf.length > 0
                         ? (selftest_nosubtle ? @"/?view=print&selftest_nosubtle=1"
                                              : @"/?view=print")
                         : @"/";

    // 打印自检轮询函数：定义留在主线程；启动时机由上面 onDidFinish 控制。
    if (selftest_pdf.length > 0) {
      auto poll_fn = print_poll_slot;
      *poll_fn = [poll_fn, web, print_bridge, selftest_pdf,
                  selftest_nosubtle](int attempts) {
        // 先取安全上下文与闸门的真实状态：不依赖数据，打印视图有没有卡片都能读到。
        [web evaluateJavaScript:@"window.__devcardFontGate ? JSON.stringify(window.__devcardFontGate()) : ''"
              completionHandler:^(id gate, NSError* gate_error) {
                NSString* gate_json = [gate isKindOfClass:NSString.class] ? gate : @"";
                if (gate_json.length > 0 && attempts == 50) {
                  NSLog(@"DEVCARD_SELFTEST font_gate=%@", gate_json);
                }
                if (selftest_nosubtle && gate_json.length > 0) {
                  // 负向自检：字体未就绪时按钮是禁用的，点按钮不会有回调，所以直接调版式入口，
                  // 断言拿到的是「校验机制不可用」这条错误，且没有产出 HTML/PDF。
                  [web evaluateJavaScript:@"window.__devcardEnsurePrintFonts()"
                        completionHandler:^(id ignored, NSError* ensure_error) {
                  dispatch_after(
                      dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)),
                      dispatch_get_main_queue(), ^{
                  [web evaluateJavaScript:
                           @"(function(){try{var r=window.__devcardExportLayout();"
                            "return JSON.stringify({error:(r&&r.error)||'',html:(r&&r.html)?r.html.length:0});}"
                            "catch(e){return JSON.stringify({thrown:String(e)});}})()"
                        completionHandler:^(id value, NSError* error) {
                          NSString* json = [value isKindOfClass:NSString.class] ? value : @"";
                          NSLog(@"DEVCARD_SELFTEST font_gate_export=%@", json.length ? json : (error.localizedDescription ?: @"nil"));
                          BOOL blocked = [json containsString:@"crypto.subtle"] &&
                                         [json containsString:@"\"html\":0"];
                          NSLog(@"DEVCARD_SELFTEST %@", blocked ? @"OK" : @"FAIL");
                          exit(blocked ? 0 : 1);
                        }];
                  });
                  }];
                  return;
                }
                [web evaluateJavaScript:@"document.querySelectorAll('.sheet').length"
                      completionHandler:^(id result, NSError* error) {
                NSInteger sheets = [result respondsToSelector:@selector(integerValue)]
                                       ? [result integerValue]
                                       : 0;
                if (sheets > 0) {
                  NSLog(@"DEVCARD_SELFTEST sheets=%ld", (long)sheets);
                  print_bridge.onExportResult = ^(BOOL ok, NSString* message) {
                    if (ok) {
                      // 直接打印通道：用同一份 PDF 构造打印任务（不弹面板）。
                      PDFDocument* document = [[PDFDocument alloc]
                          initWithURL:[NSURL fileURLWithPath:selftest_pdf]];
                      NSPrintOperation* operation =
                          [document printOperationForPrintInfo:DevCardA4PrintInfo()
                                                  scalingMode:kPDFPrintPageScaleNone
                                                   autoRotate:NO];
                      NSLog(@"DEVCARD_SELFTEST print_channel pages=%lu operation=%@",
                            (unsigned long)document.pageCount,
                            operation ? @"ok" : @"nil");
                    }
                    NSLog(@"DEVCARD_SELFTEST %@", ok ? @"OK" : @"FAIL");
                    if (!ok) {
                      NSLog(@"DEVCARD_SELFTEST error=%@", message ?: @"(无消息)");
                    }
                    exit(ok ? 0 : 1);
                  };
                  // 走真实链路：页面按钮 -> WKScriptMessageHandler -> 导出。
                  [web evaluateJavaScript:@"document.getElementById('print-export').click()"
                        completionHandler:nil];
                  return;
                }
                if (attempts <= 0) {
                  NSLog(@"DEVCARD_SELFTEST FAIL 打印视图未渲染");
                  exit(2);
                }
                dispatch_after(
                    dispatch_time(DISPATCH_TIME_NOW,
                                  (int64_t)(0.3 * NSEC_PER_SEC)),
                    dispatch_get_main_queue(), ^{
                      (*poll_fn)(attempts - 1);
                    });
              }];
              }];
      };
    }

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

    std::thread([options, server_holder, page, web]() {
      auto result =
          std::make_shared<std::unique_ptr<devcard::ServerHandle>>();
      std::string startup_error;
      try {
        *result = devcard::start_server(options);
      } catch (const std::exception& error) {
        startup_error = error.what();
      }
      DevCardStartupLog("shell_start_server");
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!*result) {
          // 启动失败（字体损坏/实例占用/端口冲突等）：沿用既有弹窗语义。
          DevCardShowFatalError(startup_error);
          [NSApp terminate:nil];
          return;
        }
        *server_holder = std::move(*result);
        NSString* url_string = [NSString
            stringWithFormat:@"http://127.0.0.1:%d%@",
                             (*server_holder)->port(), page];
        [web loadRequest:[NSURLRequest
                             requestWithURL:[NSURL
                                              URLWithString:url_string]]];
        // 打印自检不在这里启动：见上面 onDidFinish——必须等真实页导航完成。
      });
    }).detach();

    [NSApp run];
  }
  return 0;
}
