#import <Cocoa/Cocoa.h>
#import <PDFKit/PDFKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
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

// 「关于」面板：版本/构建号来自编译期注入（VERSION.txt + tag 序号），
// 与更新链路清单版本同源。链接按钮打开 OSS 手册/更新记录与 GitHub 发布页。
@interface DevCardAboutHelper : NSObject
- (void)showAbout:(id)sender;
@end

@implementation DevCardAboutHelper
- (void)showAbout:(id)sender {
  (void)sender;
  const std::string ver = devcard::current_product_version();
  const uint64_t build = devcard::compiled_build_number();
  NSString* version_line =
      build > 0
          ? [NSString stringWithFormat:@"版本 v%@（构建 %llu）",
                                       [NSString stringWithUTF8String:ver.c_str()],
                                       (unsigned long long)build]
          : [NSString stringWithFormat:@"版本 v%@",
                                        [NSString stringWithUTF8String:ver.c_str()]];
  NSAlert* alert = [[NSAlert alloc] init];
  alert.icon = NSApp.applicationIconImage;
  alert.messageText = @"DevCard 设备管理卡";
  alert.informativeText =
      [NSString stringWithFormat:@"%@\n物业设备台账与设备管理卡打印工具\n\n© 2026 小兔在线",
                                 version_line];
  [alert addButtonWithTitle:@"好"];
  [alert addButtonWithTitle:@"更新记录"];
  [alert addButtonWithTitle:@"用户手册"];
  [alert addButtonWithTitle:@"发布页"];
  NSModalResponse response = [alert runModal];
  NSString* url = nil;
  if (response == NSAlertSecondButtonReturn) {
    url = [NSString stringWithFormat:
               @"https://dl.xiaotu.online/devcard/release-notes-%@.html",
               [NSString stringWithUTF8String:ver.c_str()]];
  } else if (response == NSAlertThirdButtonReturn) {
    url = @"https://dl.xiaotu.online/devcard/manual.pdf";
  } else if (response == NSAlertFirstButtonReturn + 3) {  // 第 4 个按钮（SDK 无 Fourth 常量）
    url = @"https://github.com/LinYuwei82/dev_card_cpp/releases";
  }
  if (url) {
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
  }
}
@end

namespace {

// 启动耗时打点：从 main 入口起算的累计毫秒，与 server 侧 [startup] 日志同格式。
std::chrono::steady_clock::time_point g_startup_t0;
void StartupLog(const char* name) {
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - g_startup_t0)
                        .count();
  NSLog(@"[startup] %s = %.1fms(cum)", name, ms);
}

// 单实例：锁文件放在数据目录，第二个进程拿不到锁就激活已有窗口后退出。
// 用独立的 shell.lock，避免和服务端自己的实例锁互相影响。
int acquire_single_instance_lock(const std::string& data_dir) {
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  const std::string lock_path = data_dir + "/shell.lock";
  const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd < 0) return -1;
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return -2;
  }
  return fd;  // 进程存活期间一直持有，退出时由内核释放
}

void activate_existing_instance() {
  NSArray<NSRunningApplication*>* apps = [NSRunningApplication
      runningApplicationsWithBundleIdentifier:@"com.anye.devcard.new"];
  const pid_t self = NSProcessInfo.processInfo.processIdentifier;
  for (NSRunningApplication* app in apps) {
    if (app.processIdentifier == self) continue;
    [app activateWithOptions:NSApplicationActivateAllWindows];
    break;
  }
}

void show_error(const std::string& message) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"DevCard 启动失败";
  alert.informativeText = [NSString stringWithUTF8String:message.c_str()];
  [alert addButtonWithTitle:@"退出"];
  [alert runModal];
  [alert release];
}

// 打印统一使用 A4、零边距、100% 缩放；分页由页面里的 @page 控制。
// 导出 PDF 与直接打印共用这份设置，只有 jobDisposition 不同。
NSPrintInfo* a4_print_info() {
  NSPrintInfo* info = [NSPrintInfo.sharedPrintInfo copy];
  info.paperSize = NSMakeSize(595.28, 841.89);
  info.orientation = NSPaperOrientationPortrait;
  info.topMargin = 0;
  info.bottomMargin = 0;
  info.leftMargin = 0;
  info.rightMargin = 0;
  info.horizontallyCentered = NO;
  info.verticallyCentered = NO;
  // 调用方都是同步传给 printOperationForPrintInfo:（内部自行 retain），
  // 这里 autorelease 掉 copy 的所有权，避免每次打印/导出泄漏一份。
  return [info autorelease];
}

}  // namespace

// WKWebView 默认不处理 <a download> / Content-Disposition 下载，导出会静默失败。
// 这里把下载接到系统「下载」目录，中文文件名原样保留。
API_AVAILABLE(macos(11.0))
@interface DownloadHandler : NSObject <WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate>
@property(nonatomic, copy) void (^onDidFinish)(void);
// 文件选择面板挂在窗口上（sheet），没有窗口时退回应用级模态。
@property(nonatomic, assign) NSWindow* window;
@end

@implementation DownloadHandler

// JS 的 alert/confirm 只有实现这几个回调才会真的弹出；不实现时 WebKit 直接
// 当作用户取消（confirm 返回 false、alert 什么都不显示），删除账号 / 恢复备份
// 就会变成「点了没反应」。统一挂成窗口 sheet，modal 结束再回报结果。
- (void)presentPanelAlert:(NSAlert*)alert
               completion:(void (^)(NSModalResponse))completion {
  if (self.window != nil) {
    [alert beginSheetModalForWindow:self.window completionHandler:completion];
  } else {
    completion([alert runModal]);
  }
}

// 确认按钮按调用点语义命名：页面里的 confirm 文案已写明动作，这里只做识别，
// 认不出时退回「确定」。取消永远是默认按钮，回车不会误执行破坏性操作。
static NSString* DevCardConfirmActionTitle(NSString* message) {
  if ([message rangeOfString:@"删除"].location != NSNotFound) return @"删除";
  if ([message rangeOfString:@"覆盖"].location != NSNotFound) return @"覆盖";
  if ([message rangeOfString:@"恢复"].location != NSNotFound) return @"恢复";
  return @"确定";
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))completionHandler
    API_AVAILABLE(macos(10.12)) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"设备管理卡";
  alert.informativeText = message.length > 0 ? message : @"";
  [alert addButtonWithTitle:@"好"];
  [self presentPanelAlert:alert
               completion:^(NSModalResponse response) { completionHandler(); }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:(void (^)(BOOL))completionHandler
    API_AVAILABLE(macos(10.12)) {
  NSString* action = DevCardConfirmActionTitle(message);
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"确认操作";
  alert.informativeText = message.length > 0 ? message : @"";
  [alert addButtonWithTitle:@"取消"];
  [alert addButtonWithTitle:action];
  [self presentPanelAlert:alert completion:^(NSModalResponse response) {
    completionHandler(response == NSAlertSecondButtonReturn);
  }];
}

// 导入用的文件选择：WKWebView 不实现这个方法时，点 <input type=file> 完全无反应
// （既不弹面板也不报错）。面板由壳层出，选中后把 URL 交回 WebKit，页面照常收到
// change 事件再上传给服务端；取消则回 nil，页面什么也不做。
- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
              initiatedByFrame:(WKFrameInfo*)frame
             completionHandler:(void (^)(NSArray<NSURL*>* _Nullable))completionHandler
    API_AVAILABLE(macos(10.12)) {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.title = @"选择要导入的设备台账";
  panel.message = @"仅支持 .xlsx（导入模板由「下载模板」生成）";
  panel.prompt = @"导入";
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = NO;
  panel.allowsOtherFileTypes = NO;
  // 服务端只解析 xlsx（xlsx::read），放 csv 进来只会选完再报错。
  UTType* xlsx = [UTType typeWithFilenameExtension:@"xlsx"];
  if (xlsx != nil) panel.allowedContentTypes = @[ xlsx ];

  void (^done)(NSModalResponse) = ^(NSModalResponse result) {
    NSURL* url = (result == NSModalResponseOK) ? panel.URL : nil;
    if (url != nil && url.path.length > 0) {
      completionHandler(@[ url ]);
    } else {
      completionHandler(nil);
    }
  };
  if (self.window != nil) {
    [panel beginSheetModalForWindow:self.window completionHandler:done];
  } else {
    done([panel runModal]);
  }
}

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

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
  if (self.onDidFinish) self.onDidFinish();
}

@end

// 打印双通道：导出 PDF 与直接打印共用同一份打印 DOM，只改变输出目标。
API_AVAILABLE(macos(11.0))
@interface PrintBridge : NSObject <WKScriptMessageHandler, NSWindowDelegate, NSApplicationDelegate>
@property(nonatomic, assign) WKWebView* webView;
@property(nonatomic, assign) NSWindow* window;
// 自动化验证：设置后跳过保存面板，直接把 PDF 写到该路径。
@property(nonatomic, copy) NSString* automaticPdfPath;
@property(nonatomic, copy) void (^onExportResult)(BOOL ok, NSString* message);
// 打印期间持有 PDFDocument，避免任务释放后文档被回收。
@property(nonatomic, retain) PDFDocument* printDocument;
// Sparkle 自更新桥（从 shell/mac/updater.h 引入；桥内部持有 controller + delegate）。
@property(nonatomic, strong) DevCardUpdaterBridge* updaterBridge;
// 「关于 DevCard」面板处理器（状态栏版本号点击走 showAbout 时复用）。
@property(nonatomic, assign, nullable) DevCardAboutHelper* aboutHandler;
@end

@implementation PrintBridge

// 点窗口红叉（关闭按钮）时，关闭最后一个窗口 = 终止 App。
// 没有这个方法时，红叉只关窗口但 NSApp 继续跑（留在 Dock 里），
// 用户必须 Cmd+Q 或 Dock 右键才能真正退出——这是用户反馈的根因。
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}

// 关闭主窗口时也直接 terminate，不等 applicationShouldTerminateAfterLastWindowClosed:
// 的下一轮事件循环——避免窗口已消失但 Dock 图标还闪一下。
- (BOOL)windowShouldClose:(id)sender {
  [NSApp terminate:nil];
  return YES;
}

- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if (![message.body isKindOfClass:[NSDictionary class]]) return;
  NSDictionary* body = (NSDictionary*)message.body;
  NSString* action = body[@"action"];
  if ([action isEqualToString:@"exportPdf"]) {
    [self exportPdfNamed:body[@"filename"]];
  } else if ([action isEqualToString:@"print"]) {
    [self printCards];
  } else if ([action isEqualToString:@"saveFile"]) {
    [self saveFileNamed:body[@"filename"]
                  title:body[@"title"]
                 base64:body[@"contentBase64"]];
  } else if ([action isEqualToString:@"pickSavePath"]) {
    // 只让用户选落点，不下发字节：更新包由服务端按这个路径落盘（大文件不转 base64）。
    [self pickSavePathNamed:body[@"filename"] title:body[@"title"]];
  } else if ([action isEqualToString:@"openDataDir"]) {
    NSString* path = body[@"path"];
    if ([path isKindOfClass:[NSString class]] && path.length > 0) {
      [[NSWorkspace sharedWorkspace]
          openURL:[NSURL fileURLWithPath:path isDirectory:YES]];
    }
  } else if ([action isEqualToString:@"openExternal"]) {
    // 版本检查的「下载页」跳转：只放行 http/https，不做任何下载动作。
    NSString* value = body[@"url"];
    NSURL* target = [value isKindOfClass:[NSString class]]
                        ? [NSURL URLWithString:value]
                        : nil;
    NSString* scheme = target.scheme.lowercaseString;
    if (target && ([scheme isEqualToString:@"https"] ||
                   [scheme isEqualToString:@"http"])) {
      [[NSWorkspace sharedWorkspace] openURL:target];
    } else {
      [self notify:@"下载页地址无效" kind:@"error"];
    }
  } else if ([action isEqualToString:@"showAbout"]) {
    // 状态栏版本号点击：与 App 菜单「关于 DevCard」同一个面板。
    [self.aboutHandler showAbout:nil];
  } else if ([action isEqualToString:@"checkForUpdates"]) {
    // 原生更新引擎：前端桥触发 core UpdateEngine 立即检查（与菜单同一入口）。
    if (![self.updaterBridge checkForUpdates]) {
      [self notify:@"更新检查未配置（缺少 SUFeedURL / SUPublicEDKey）" kind:@"error"];
    }
  } else if ([action isEqualToString:@"quit"]) {
    // 【回归 Bug 修复｜请勿改动】页面顶栏「退出」在桌面版等于退出应用。
    // 前端的 el.logout 会先尝试走这条桥；如果这里被删掉，点击「退出」会静默
    // 落回前端的注销分支，停在登录页（历史上已经因此产生两次重复反馈）。
    [NSApp terminate:nil];
  }
}

// 页面导出 xlsx（设备台账 / 导入模板）统一走这里：内容由页面取好（带会话），
// 壳层只负责「问路径 + 原子写入」。取消时不写任何字节，也不弹提示，
// 只把 cancel 回给页面用于恢复按钮状态——与小兔进销存的导出桥同一口径。
- (void)saveFileNamed:(NSString*)name
                title:(NSString*)title
               base64:(NSString*)base64 {
  NSData* data = base64.length > 0
                     ? [[NSData alloc] initWithBase64EncodedString:base64 options:0]
                     : nil;
  if (data.length == 0) {
    [self finishSave:@"error" path:nil message:@"导出内容为空"];
    return;
  }
  NSString* filename = name.length > 0 ? name : @"设备台账.xlsx";
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.title = title.length > 0 ? title : @"保存文件";
  panel.nameFieldStringValue = filename;
  panel.canCreateDirectories = YES;
  NSString* ext = filename.pathExtension.lowercaseString;
  if (ext.length > 0) {
    UTType* type = [UTType typeWithFilenameExtension:ext];
    if (type != nil) panel.allowedContentTypes = @[ type ];
  }

  void (^done)(NSModalResponse) = ^(NSModalResponse result) {
    if (result != NSModalResponseOK || panel.URL == nil) {
      [self finishSave:@"cancel" path:nil message:nil];
      return;
    }
    NSURL* url = panel.URL;
    NSError* error = nil;
    if (![data writeToURL:url options:NSDataWritingAtomic error:&error]) {
      [self finishSave:@"error"
                  path:url.path
               message:error.localizedDescription ?: @"无法写入目标文件"];
      return;
    }
    [self finishSave:@"ok" path:url.path message:nil];
  };
  if (self.window != nil) {
    [panel beginSheetModalForWindow:self.window completionHandler:done];
  } else {
    done([panel runModal]);
  }
}

- (void)pickSavePathNamed:(NSString*)name title:(NSString*)title {
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.title = title.length > 0 ? title : @"保存文件";
  panel.nameFieldStringValue =
      name.length > 0 ? name : @"DevCard-update";
  panel.canCreateDirectories = YES;
  // 默认落在用户的下载目录（符合大多数人的预期）；没有下载目录就退到文稿，
  // 绝不默认写进应用自己的数据目录或临时目录。
  NSString* dir = NSSearchPathForDirectoriesInDomains(
      NSDownloadsDirectory, NSUserDomainMask, YES).firstObject;
  if (dir.length == 0 ||
      ![[NSFileManager defaultManager] fileExistsAtPath:dir]) {
    dir = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  }
  if (dir.length > 0) {
    panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
  }
  NSString* ext = panel.nameFieldStringValue.pathExtension.lowercaseString;
  if (ext.length > 0) {
    UTType* type = [UTType typeWithFilenameExtension:ext];
    if (type != nil) panel.allowedContentTypes = @[ type ];
  }
  void (^done)(NSModalResponse) = ^(NSModalResponse result) {
    if (result != NSModalResponseOK || panel.URL == nil) {
      [self finishPath:@"cancel" path:nil];
      return;
    }
    [self finishPath:@"ok" path:panel.URL.path];
  };
  if (self.window != nil) {
    [panel beginSheetModalForWindow:self.window completionHandler:done];
  } else {
    done([panel runModal]);
  }
}

- (void)finishPath:(NSString*)status path:(NSString*)path {
  NSMutableDictionary* payload = [NSMutableDictionary dictionary];
  payload[@"status"] = status ?: @"cancel";
  if (path.length > 0) payload[@"path"] = path;
  NSData* data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
  NSString* json = data != nil
                       ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                       : @"{}";
  NSString* js = [NSString
      stringWithFormat:@"window.__devcardPathPicked && window.__devcardPathPicked(%@);",
                       json];
  [self.webView evaluateJavaScript:js completionHandler:nil];
}

- (void)finishSave:(NSString*)status path:(NSString*)path message:(NSString*)message {
  NSMutableDictionary* payload = [NSMutableDictionary dictionary];
  payload[@"status"] = status ?: @"error";
  if (path.length > 0) payload[@"path"] = path;
  if (message.length > 0) payload[@"message"] = message;
  NSData* data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
  NSString* json = data != nil
                       ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                       : @"{}";
  NSString* js = [NSString
      stringWithFormat:@"window.__devcardSaveResult && window.__devcardSaveResult(%@);",
                       json];
  [self.webView evaluateJavaScript:js completionHandler:nil];
}

- (void)exportPdfNamed:(NSString*)suggestedName {  NSString* name = suggestedName.length > 0 ? suggestedName : @"设备管理卡.pdf";
  // 先把页面切成 1:1、无工具栏的导出布局，再逐张 A4 截取。
  [self.webView
      evaluateJavaScript:@"window.__devcardExportLayout ? window.__devcardExportLayout() : null"
       completionHandler:^(id result, NSError* error) {
         NSDictionary* layout =
             [result isKindOfClass:[NSDictionary class]] ? result : nil;
         // 页面侧字体没就绪时只回一个 error，不返回 rects：这里必须中止，不能落到系统字体。
         if ([layout[@"error"] length] > 0) {
           [self finishExport:NO message:layout[@"error"]];
           return;
         }
         NSArray* rects = layout[@"rects"];
         if (rects.count == 0) {
           [self finishExport:NO message:@"没有可导出的卡片"];
           return;
         }

         NSURL* url = nil;
         if (self.automaticPdfPath.length > 0) {
           url = [NSURL fileURLWithPath:self.automaticPdfPath];
         } else {
           NSSavePanel* panel = [NSSavePanel savePanel];
           panel.title = @"导出卡片 PDF";
           panel.nameFieldStringValue = name;
           panel.allowedContentTypes = @[ UTTypePDF ];
           panel.canCreateDirectories = YES;
           if ([panel runModal] != NSModalResponseOK) {
             [self.webView evaluateJavaScript:@"window.__devcardExportDone && window.__devcardExportDone()"
                                   completionHandler:nil];
             [self finishExport:NO message:@"已取消导出"];
             return;
           }
           url = panel.URL;
         }

         NSLog(@"[print] 导出布局 sheets=%lu", (unsigned long)rects.count);
         [self buildPdfFromRects:rects
                      completion:^(NSData* pdf, NSInteger pages, NSString* error) {
                        if (pdf.length == 0) {
                          [self finishExport:NO message:error ?: @"导出 PDF 失败"];
                          return;
                        }
                        if (![pdf writeToURL:url atomically:YES]) {
                          [self finishExport:NO message:@"导出 PDF 失败（无法写入目标文件）"];
                          return;
                        }
                        [self finishExport:YES
                                   message:[NSString
                                               stringWithFormat:@"已导出 %ld 页 PDF：%@",
                                                                (long)pages, url.path]];
                      }];
       }];
}

// 逐张截取打印页，再合成一个 A4 PDF；导出与直接打印复用同一份数据。
- (void)buildPdfFromRects:(NSArray*)rects
               completion:(void (^)(NSData* pdf, NSInteger pages, NSString* error))completion {
  [self captureRects:rects index:0 collected:[NSMutableArray array] completion:completion];
}

- (void)captureRects:(NSArray*)rects
               index:(NSInteger)index
           collected:(NSMutableArray*)collected
          completion:(void (^)(NSData* pdf, NSInteger pages, NSString* error))completion {
  if (index >= (NSInteger)rects.count) {
    [self.webView evaluateJavaScript:@"window.__devcardExportDone && window.__devcardExportDone()"
                          completionHandler:nil];
    NSString* failure = nil;
    NSData* pdf = [self pdfDataFromPages:collected error:&failure];
    completion(pdf, (NSInteger)collected.count, failure);
    return;
  }

  NSDictionary* rect = rects[index];
  WKPDFConfiguration* config = [[WKPDFConfiguration alloc] init];
  config.rect = CGRectMake([rect[@"x"] doubleValue], [rect[@"y"] doubleValue],
                           [rect[@"width"] doubleValue],
                           [rect[@"height"] doubleValue]);
  [self.webView
      createPDFWithConfiguration:config
               completionHandler:^(NSData* data, NSError* error) {
                 if (data.length == 0) {
                   [self.webView
                       evaluateJavaScript:@"window.__devcardExportDone && window.__devcardExportDone()"
                            completionHandler:nil];
                   completion(nil, 0, error.localizedDescription ?: @"导出 PDF 失败（无数据）");
                   return;
                 }
                 [collected addObject:data];
                 [self captureRects:rects
                              index:index + 1
                          collected:collected
                         completion:completion];
               }];
  [config release];
}

// 每张截取结果是 210mm × 297mm（CSS px），按 A4 点尺寸缩放后逐页写入内存。
- (NSData*)pdfDataFromPages:(NSArray*)pages error:(NSString**)error {
  const CGFloat a4Width = 595.276;
  const CGFloat a4Height = 841.89;
  CGRect media_box = CGRectMake(0, 0, a4Width, a4Height);

  CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
  CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData(data);
  CGContextRef context = CGPDFContextCreate(consumer, &media_box, NULL);
  CGDataConsumerRelease(consumer);
  if (!context) {
    CFRelease(data);
    if (error) *error = @"导出 PDF 失败（无法创建 PDF 上下文）";
    return nil;
  }

  for (NSData* page_data in pages) {
    CGDataProviderRef provider =
        CGDataProviderCreateWithCFData((__bridge CFDataRef)page_data);
    CGPDFDocumentRef document = CGPDFDocumentCreateWithProvider(provider);
    CGDataProviderRelease(provider);
    CGPDFPageRef page = document ? CGPDFDocumentGetPage(document, 1) : NULL;
    if (!page) {
      if (document) CGPDFDocumentRelease(document);
      CGPDFContextClose(context);
      CGContextRelease(context);
      CFRelease(data);
      if (error) *error = @"导出 PDF 失败（无法解析 PDF 数据）";
      return nil;
    }

    const CGRect capture = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
    const CGFloat scale = a4Width / capture.size.width;
    CGPDFContextBeginPage(context, NULL);
    CGContextSaveGState(context);
    CGContextScaleCTM(context, scale, scale);
    CGContextDrawPDFPage(context, page);
    CGContextRestoreGState(context);
    CGPDFContextEndPage(context);

    CGPDFDocumentRelease(document);
  }

  CGPDFContextClose(context);
  CGContextRelease(context);
  return [(__bridge NSData*)data autorelease];
}

- (void)finishExport:(BOOL)ok message:(NSString*)message {
  NSLog(@"[print] %@", message);
  [self notify:message kind:ok ? nil : @"error"];
  if (self.onExportResult) self.onExportResult(ok, message);
}

- (void)printCards {
  // 没装打印机时打印面板会一直等不到纸张尺寸，先给出明确提示。
  NSString* printer_name = [NSPrintInfo.sharedPrintInfo.printer.name
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  if (printer_name.length == 0) {
    [self notify:@"系统未添加打印机，请先在「系统设置 › 打印机与扫描仪」中添加，或改用导出 PDF"
            kind:@"error"];
    return;
  }

  // 直接打印复用导出 PDF 的同一份 A4 数据，避免 WebKit 打印会话的版式差异。
  [self.webView
      evaluateJavaScript:@"window.__devcardExportLayout ? window.__devcardExportLayout() : null"
       completionHandler:^(id result, NSError* error) {
         NSDictionary* layout =
             [result isKindOfClass:[NSDictionary class]] ? result : nil;
         if ([layout[@"error"] length] > 0) {
           [self notify:layout[@"error"] kind:@"error"];
           return;
         }
         NSArray* rects = layout[@"rects"];
         if (rects.count == 0) {
           [self notify:@"没有可打印的卡片" kind:@"error"];
           return;
         }
         [self buildPdfFromRects:rects
                      completion:^(NSData* pdf, NSInteger pages, NSString* error) {
                        if (pdf.length == 0) {
                          [self notify:error ?: @"无法生成打印数据" kind:@"error"];
                          return;
                        }
                        PDFDocument* document = [[PDFDocument alloc] initWithData:pdf];
                        if (!document) {
                          [self notify:@"无法生成打印数据" kind:@"error"];
                          return;
                        }
                        NSPrintOperation* operation =
                            [document printOperationForPrintInfo:a4_print_info()
                                                    scalingMode:kPDFPrintPageScaleNone
                                                     autoRotate:NO];
                        if (!operation) {
                          [self notify:@"无法创建打印任务" kind:@"error"];
                          return;
                        }
                        self.printDocument = document;
                        operation.showsPrintPanel = YES;
                        operation.showsProgressPanel = YES;
                        [operation
                            runOperationModalForWindow:self.window
                                              delegate:self
                                        didRunSelector:
                                            @selector(printOperationDidRun:success:contextInfo:)
                                           contextInfo:NULL];
                      }];
       }];
}

- (void)printOperationDidRun:(NSPrintOperation*)operation
                     success:(BOOL)success
                 contextInfo:(void*)contextInfo {
  self.printDocument = nil;
  if (!success) [self notify:@"打印已取消或失败" kind:nil];
}

- (NSString*)jsLiteral:(NSString*)value {
  NSData* data = [NSJSONSerialization dataWithJSONObject:@[ value ?: @"" ]
                                                 options:0
                                                   error:nil];
  NSString* json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
  return [json substringWithRange:NSMakeRange(1, json.length - 2)];
}

- (void)notify:(NSString*)message kind:(NSString*)kind {
  if (!self.webView) return;
  NSString* js = [NSString stringWithFormat:
      @"window.__devcardNotify && window.__devcardNotify(%@, %@);",
      kind ? [self jsLiteral:kind] : @"null", [self jsLiteral:message]];
  [self.webView evaluateJavaScript:js completionHandler:nil];
}

@end


int main(int argc, const char* argv[]) {
  g_startup_t0 = std::chrono::steady_clock::now();
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    devcard::ServerOptions options;
    options.data_dir = devcard::default_data_dir();
    options.port = 18080;

    const int instance_lock = acquire_single_instance_lock(options.data_dir);
    if (instance_lock == -2) {
      // 已有实例：把它叫到前台，本进程直接退出，不弹错误框。
      activate_existing_instance();
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
    static DevCardAboutHelper* g_about_handler = [DevCardAboutHelper new];
    NSMenuItem* about_item =
        [app_menu addItemWithTitle:@"关于 DevCard"
                             action:@selector(showAbout:)
                      keyEquivalent:@""];
    about_item.target = g_about_handler;
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
    PrintBridge* bridge = [[PrintBridge alloc] init];
    bridge.updaterBridge = [[DevCardUpdaterBridge alloc] init];
    bridge.updaterBridge.checkUpdateMenuItem = check_update_item_placeholder;
    // 状态栏版本号点击（showAbout）复用同一个关于处理器。
    bridge.aboutHandler = g_about_handler;
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
    DownloadHandler* download_handler = [[DownloadHandler alloc] init];
    download_handler.window = window;
    // 打印自检启动钩子：必须等真实页（http://）didFinish 之后再发起 JS——
    // splash→真实页导航进行中时 evaluateJavaScript 会被 WebKit 竞态吞掉。
    // 轮询函数用 std::function + shared_ptr 自引用：ObjC 块递归 + 在 GCD
    // 上下文拷贝块捕获的写法实测触发 _Block_copy(NULL) 段错误（DevCard-*.ips）。
    auto print_poll_slot = std::make_shared<std::function<void(int)>>();
    __block BOOL print_poll_started = NO;
    download_handler.onDidFinish = ^{
      StartupLog("webview_did_finish");
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
    // 关键：设 window.delegate，否则 PrintBridge 的 NSWindowDelegate 方法
    // （比如 applicationShouldTerminateAfterLastWindowClosed）不会被调用。
    window.delegate = bridge;
    [NSApp setDelegate:bridge];

    // Sparkle 2 自更新：从 Info.plist 读 SUFeedURL + SUPublicEDKey 初始化，
    // 同时绑定 App 菜单 target + 处理 DEVCARD_SPARKLE_SELFTEST。
    // 全部逻辑抽到 shell/mac/updater.mm 的 DevCardUpdaterBridge。
    [bridge.updaterBridge start];

    // 自动化验证：把打印视图导出成 PDF 后退出，便于脚本核对页数与文字。
    NSString* selftest_pdf =
        NSProcessInfo.processInfo.environment[@"DEVCARD_PRINT_SELFTEST_PDF"];    bridge.automaticPdfPath = selftest_pdf;
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
      *poll_fn = [poll_fn, web, bridge, selftest_pdf,
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
                  bridge.onExportResult = ^(BOOL ok, NSString* message) {
                    if (ok) {
                      // 直接打印通道：用同一份 PDF 构造打印任务（不弹面板）。
                      PDFDocument* document = [[PDFDocument alloc]
                          initWithURL:[NSURL fileURLWithPath:selftest_pdf]];
                      NSPrintOperation* operation =
                          [document printOperationForPrintInfo:a4_print_info()
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
    StartupLog("shell_window_visible");

    std::thread([options, server_holder, page, web]() {
      auto result =
          std::make_shared<std::unique_ptr<devcard::ServerHandle>>();
      std::string startup_error;
      try {
        *result = devcard::start_server(options);
      } catch (const std::exception& error) {
        startup_error = error.what();
      }
      StartupLog("shell_start_server");
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!*result) {
          // 启动失败（字体损坏/实例占用/端口冲突等）：沿用既有弹窗语义。
          show_error(startup_error);
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
