#import <Cocoa/Cocoa.h>
#import <PDFKit/PDFKit.h>
#import <Sparkle/Sparkle.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
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
  return info;
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
// Sparkle 自更新控制器：Info.plist 里 SUFeedURL + SUPublicEDKey 都有值才初始化。
@property(nonatomic, strong) SPUStandardUpdaterController* updaterController;
// Sparkle 代理：把 feed 拉取/版本比较/安装结果写到 NSLog，便于端到端实测取证。
@property(nonatomic, strong) id<SPUUpdaterDelegate> updaterDelegate;
// App 菜单里的「检查更新…」项，Sparkle 启用后把 target 绑定到 updaterController。
@property(nonatomic, assign) NSMenuItem* checkUpdateMenuItem;
@end

// Sparkle 更新事件日志代理。只做观测，不改变 Sparkle 的默认行为。
@interface DevCardUpdaterDelegate : NSObject <SPUUpdaterDelegate>
@end

@implementation DevCardUpdaterDelegate

- (void)updater:(SPUUpdater*)updater didFindValidUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] didFindValidUpdate version=%@ display=%@ url=%@",
        item.versionString, item.displayVersionString, item.fileURL);
}

- (void)updaterDidNotFindUpdate:(SPUUpdater*)updater error:(NSError*)error {
  NSLog(@"[sparkle] didNotFindUpdate error=%@", error.localizedDescription);
}

- (void)updater:(SPUUpdater*)updater
    didFailToFetchAppcastWithError:(NSError*)error {
  NSLog(@"[sparkle] didFailToFetchAppcast error=%@ code=%ld",
        error.localizedDescription, (long)error.code);
}

- (void)updater:(SPUUpdater*)updater didDownloadUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] didDownloadUpdate version=%@", item.versionString);
}

- (void)updater:(SPUUpdater*)updater willInstallUpdate:(SUAppcastItem*)item {
  NSLog(@"[sparkle] willInstallUpdate version=%@", item.versionString);
}

- (void)updater:(SPUUpdater*)updater didAbortWithError:(NSError*)error {
  NSLog(@"[sparkle] didAbortWithError error=%@ code=%ld",
        error.localizedDescription, (long)error.code);
}

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
  } else if ([action isEqualToString:@"checkForUpdates"]) {
    // Sparkle 自更新：前端用户点「安装并重启」时转调 Sparkle。
    // 开发期没有 SUFeedURL / SUPublicEDKey，updaterController 是 nil——
    // 这种情况 fallback 到默认动作：让用户从菜单手动检查。
    if (self.updaterController) {
      [self.updaterController checkForUpdates:body[@"silent"] ? nil : nil];
    } else {
      [self notify:@"Sparkle 未启用（缺少签名公钥或 feed URL）" kind:@"error"];
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
  NSString* printer_name = [[NSPrintInfo.sharedPrintInfo.printer.name
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] copy];
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

// 自动更新自检：四种失败路径（超时 / DNS 非 JSON / 版本号非法）必须全静默。
// 状态放文件级变量而不是 __block：跨回调的 __block 状态在本项目第一次实现时直接
// SIGSEGV（dispatch 回调里调到了空 block），换成静态变量后不再依赖编译器对
// ObjC++ 下 __block 的处理。
static WKWebView* g_update_selftest_web = nil;
static BOOL g_update_selftest_started = NO;

// replay 模式：把已抓到的真实 Release 响应体喂进页面的解析/比较/提示链路，
// 再用 WKWebView 自己的快照落 PNG——不经过窗口层级，绕开「Trae 永远在前」的问题。
static void devcard_poll_replay(int attempts) {
  WKWebView* web = g_update_selftest_web;
  NSString* fixture = NSProcessInfo.processInfo.environment[@"DEVCARD_UPDATE_FIXTURE"];
  if (fixture.length == 0) {
    fixture = @"/tmp/m6_release_fixture.json";
  }
  NSString* body = [NSString stringWithContentsOfFile:fixture
                                             encoding:NSUTF8StringEncoding
                                                error:nil];
  if (!web || body.length == 0) {
    NSLog(@"DEVCARD_SELFTEST replay_fail 读不到 fixture：%@", fixture);
    exit(2);
  }
  [web evaluateJavaScript:@"typeof window.__devcardUpdateReplay"
        completionHandler:^(id value, NSError* error) {
          NSString* kind = [value isKindOfClass:NSString.class] ? value : @"";
          if (![kind isEqualToString:@"function"]) {
            if (attempts <= 0) {
              NSLog(@"DEVCARD_SELFTEST replay_fail 页面未就绪");
              exit(2);
            }
            dispatch_after(
                dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                dispatch_get_main_queue(), ^{
                  devcard_poll_replay(attempts - 1);
                });
            return;
          }
          NSData* encoded = [NSJSONSerialization dataWithJSONObject:@[ body ]
                                                           options:0
                                                             error:nil];
          NSString* literal = [[NSString alloc] initWithData:encoded
                                                    encoding:NSUTF8StringEncoding];
          NSString* call =
              [NSString stringWithFormat:@"window.__devcardUpdateReplay((%@)[0])", literal];
          [web evaluateJavaScript:call
                completionHandler:^(id result, NSError* call_error) {
                  NSString* summary =
                      [result isKindOfClass:NSString.class] ? result : @"{}";
                  NSLog(@"DEVCARD_SELFTEST replay=%@", summary);
                  [web evaluateJavaScript:
                           @"(function(){var p=document.getElementById('update-pill');"
                            "if(!p||p.hidden)return 'no-pill';p.click();return 'clicked';})()"
                        completionHandler:^(id clicked, NSError* click_error) {
                          NSLog(@"DEVCARD_SELFTEST replay_ui=%@",
                                [clicked isKindOfClass:NSString.class] ? clicked : @"?");
                          dispatch_after(
                              dispatch_time(DISPATCH_TIME_NOW,
                                            (int64_t)(0.8 * NSEC_PER_SEC)),
                              dispatch_get_main_queue(), ^{
                                WKSnapshotConfiguration* config =
                                    [[WKSnapshotConfiguration alloc] init];
                                [web takeSnapshotWithConfiguration:config
                                                 completionHandler:^(NSImage* image,
                                                                     NSError* snap_error) {
                                                   if (image == nil) {
                                                     NSLog(@"DEVCARD_SELFTEST replay_fail 快照失败：%@",
                                                           snap_error.localizedDescription
                                                               ?: @"(无图)");
                                                   } else {
                                                     NSBitmapImageRep* rep =
                                                         [NSBitmapImageRep imageRepWithData:
                                                                               [image TIFFRepresentation]];
                                                     NSData* png = [rep representationUsingType:
                                                                            NSBitmapImageFileTypePNG
                                                                                    properties:@{}];
                                                     NSString* png_path =
                                                         @"/tmp/m6_update_ui.png";
                                                     [png writeToFile:png_path atomically:YES];
                                                     NSLog(@"DEVCARD_SELFTEST replay_shot=%@ (%lu B)",
                                                           png_path,
                                                           (unsigned long)png.length);
                                                   }
                                                   [summary writeToFile:@"/tmp/m6_update_replay.json"
                                                             atomically:YES
                                                               encoding:NSUTF8StringEncoding
                                                                  error:nil];
                                                   BOOL ok =
                                                       [summary rangeOfString:@"\"ok\":true"]
                                                           .location != NSNotFound;
                                                   NSLog(@"DEVCARD_SELFTEST %@",
                                                         ok ? @"OK" : @"FAIL");
                                                   exit(ok ? 0 : 1);
                                                 }];
                              });
                        }];
                }];
        }];
}

static void devcard_poll_update_step(int attempts);

// 超时诊断再进一步：直接问页面「你自己的 /api/version 拿不拿得到」。
// 更新检查失败只报 Load failed，分不清是会话丢了，还是请求被拦在网络上。
static void devcard_live_api_probe(WKWebView* web, int attempts, NSString* out_path) {
  NSString* js =
      @"(function(){"
       "if(!window.__devcardApiProbe){"
       "window.__devcardApiProbe='pending';"
       "fetch('/api/version',{cache:'no-store',credentials:'same-origin'})"
       ".then(function(r){return r.text().then(function(t){"
       "window.__devcardApiProbe='HTTP '+r.status+' '+t.slice(0,160);});})"
       ".catch(function(e){window.__devcardApiProbe='ERR '+e.name+': '+e.message;});}"
       "return window.__devcardApiProbe;})()";
  [web evaluateJavaScript:js
        completionHandler:^(id value, NSError* error) {
          NSString* probe = [value isKindOfClass:NSString.class] ? value : @"?";
          if ([probe isEqualToString:@"pending"] && attempts > 0) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                         (int64_t)(0.3 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{
                             devcard_live_api_probe(web, attempts - 1, out_path);
                           });
            return;
          }
          [web evaluateJavaScript:@"(window.__devcardUpdateDiag ? window.__devcardUpdateDiag() : '{}')"
                completionHandler:^(id diag, NSError* diag_error) {
                  NSString* body = [diag isKindOfClass:NSString.class] ? diag : @"{}";
                  NSData* encoded = [NSJSONSerialization dataWithJSONObject:@[ probe ]
                                                                   options:0
                                                                     error:nil];
                  NSString* quoted = [[NSString alloc] initWithData:encoded
                                                          encoding:NSUTF8StringEncoding];
                  NSString* text_out = [NSString
                      stringWithFormat:@"{\"status\":\"no-result\",\"api_probe\":%@,"
                                       @"\"diag\":%@}",
                                       [quoted substringWithRange:NSMakeRange(1, quoted.length - 2)],
                                       body];
                  [text_out writeToFile:out_path
                              atomically:YES
                                encoding:NSUTF8StringEncoding
                                   error:nil];
                  NSLog(@"DEVCARD_SELFTEST update_live=%@", text_out);
                  exit(2);
                }];
        }];
}

// live 模式：不替换传输层，直接打真实发布接口，把请求地址、响应原文长度与片段、
// 解析结果打印出来。留在前台不退出，便于同时截屏——验收记录要的是「真实元数据 + 界面」。
static void devcard_poll_update_live(int attempts) {
  WKWebView* web = g_update_selftest_web;
  if (!web) {
    NSLog(@"DEVCARD_SELFTEST update_fail 没有页面");
    exit(2);
  }
  NSString* probe =
      @"(function(){"
       "var s=window.__devcardUpdateState?window.__devcardUpdateState():null;"
       "var c=s&&s.check;"
       "if(!c||!c.status)return '';"
       "return JSON.stringify({version:s.version,pill:s.pill,endpoint:c.endpoint||'',"
       "status:c.status||'',rawLen:(c.raw||'').length,rawHead:(c.raw||'').slice(0,200),"
       "release:c.release||null,detail:c.detail||''});})()";
  [web evaluateJavaScript:probe
        completionHandler:^(id value, NSError* error) {
          NSString* text = [value isKindOfClass:NSString.class] ? value : @"";
          if (text.length == 0) {
            if (attempts <= 0) {
              // 把中间状态写盘：只报「超时」不足以判断卡在哪一步。
              NSString* out_path =
                  NSProcessInfo.processInfo.environment[@"DEVCARD_SELFTEST_UPDATE_OUT"];
              if (out_path.length == 0) {
                out_path = @"/tmp/m6_update_live.json";
              }
              devcard_live_api_probe(web, 20, out_path);
              return;
            }
            dispatch_after(
                dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                dispatch_get_main_queue(), ^{
                  devcard_poll_update_live(attempts - 1);
                });
            return;
          }
          NSLog(@"DEVCARD_SELFTEST update_live=%@", text);
          // 同时落盘：NSLog 在输出重定向时会被缓冲，进程被清理就什么都不剩。
          NSString* out_path =
              NSProcessInfo.processInfo.environment[@"DEVCARD_SELFTEST_UPDATE_OUT"];
          if (out_path.length == 0) {
            out_path = @"/tmp/m6_update_live.json";
          }
          [text writeToFile:out_path atomically:YES
                   encoding:NSUTF8StringEncoding
                      error:nil];
          // 有提示就把弹窗打开，供截屏；没有提示也把状态打完，别伪装成成功。
          [web evaluateJavaScript:
                   @"(function(){var p=document.getElementById('update-pill');"
                    "if(!p||p.hidden)return 'no-pill';p.click();return 'clicked';})()"
                completionHandler:^(id clicked, NSError* click_error) {
                  NSString* ui =
                      [clicked isKindOfClass:NSString.class] ? clicked : @"?";
                  NSLog(@"DEVCARD_SELFTEST update_live_ui=%@", ui);
                  if (![ui isEqualToString:@"clicked"]) {
                    NSLog(@"DEVCARD_SELFTEST live_ready");
                    exit(3);
                    return;
                  }
                  // 弹窗打开后等一下再拍：快照拿到的是 WebView 自己渲染的位图，
                  // 不经过窗口层级，Trae 挡在前面也不影响。
                  dispatch_after(
                      dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)),
                      dispatch_get_main_queue(), ^{
                        WKSnapshotConfiguration* shot_config =
                            [[WKSnapshotConfiguration alloc] init];
                        [web takeSnapshotWithConfiguration:shot_config
                                         completionHandler:^(NSImage* image,
                                                             NSError* snap_error) {
                                           NSString* png_path =
                                               NSProcessInfo.processInfo.environment
                                                   [@"DEVCARD_SELFTEST_UPDATE_SHOT"];
                                           if (png_path.length == 0) {
                                             png_path = @"/tmp/m6_update_live_ui.png";
                                           }
                                           if (image == nil) {
                                             NSLog(@"DEVCARD_SELFTEST live_shot_fail=%@",
                                                   snap_error.localizedDescription
                                                       ?: @"(无图)");
                                             exit(4);
                                             return;
                                           }
                                           NSBitmapImageRep* rep =
                                               [NSBitmapImageRep imageRepWithData:
                                                                     [image TIFFRepresentation]];
                                           NSData* png = [rep representationUsingType:
                                                                  NSBitmapImageFileTypePNG
                                                                          properties:@{}];
                                           [png writeToFile:png_path atomically:YES];
                                           NSLog(@"DEVCARD_SELFTEST live_shot=%@ (%lu B)",
                                                 png_path, (unsigned long)png.length);
                                           exit(0);
                                         }];
                      });
                }];
        }];
}

static void devcard_poll_update_later(int attempts) {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   devcard_poll_update_step(attempts);
                 });
}

static void devcard_poll_update_step(int attempts) {
  WKWebView* web = g_update_selftest_web;
  if (!web) {
    NSLog(@"DEVCARD_SELFTEST update_fail 没有页面");
    exit(2);
  }
  NSString* probe = g_update_selftest_started
                        ? @"window.__devcardUpdateSelftestResult ? "
                           "JSON.stringify(window.__devcardUpdateSelftestResult) : ''"
                        : @"typeof window.__devcardUpdateSelftestRun";
  [web evaluateJavaScript:probe
        completionHandler:^(id value, NSError* error) {
          NSString* text = [value isKindOfClass:NSString.class] ? value : @"";
          if (!g_update_selftest_started) {
            if ([text isEqualToString:@"function"]) {
              g_update_selftest_started = YES;
              [web evaluateJavaScript:@"window.__devcardUpdateSelftestRun()"
                    completionHandler:nil];
            }
          } else if (text.length > 0) {
            NSLog(@"DEVCARD_SELFTEST update=%@", text);
            BOOL all_ok = [text rangeOfString:@"\"ok\":false"].location == NSNotFound;
            NSLog(@"DEVCARD_SELFTEST %@", all_ok ? @"OK" : @"FAIL");
            exit(all_ok ? 0 : 1);
          }
          if (attempts <= 0) {
            NSLog(@"DEVCARD_SELFTEST update_fail 自检超时（started=%@）",
                  g_update_selftest_started ? @"YES" : @"NO");
            exit(2);
          }
          devcard_poll_update_later(attempts - 1);
        }];
}

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

    // Sparkle 自动更新菜单项：target 先占位，updaterController 初始化后再绑定。
    // updaterController 为 nil 时（开发期无 SUFeedURL / SUPublicEDKey）把它隐藏。
    NSMenuItem* check_update_item =
        [app_menu addItemWithTitle:@"检查更新…"
                             action:@selector(checkForUpdates:)
                      keyEquivalent:@""];
    check_update_item.target = nil;  // 稍后由 updaterController 接管

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

    // 自检要的是「一次真实请求」，而正式节流是 24 小时：自检模式只清掉
    // 「上次检查时间」这一个 key，不动登录态（换非持久存储会丢会话，接口直接 401）。
    NSString* selftest_update_mode =
        NSProcessInfo.processInfo.environment[@"DEVCARD_SELFTEST_UPDATE"];

    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    WKUserContentController* controller = [[WKUserContentController alloc] init];
    PrintBridge* bridge = [[PrintBridge alloc] init];
    bridge.checkUpdateMenuItem = check_update_item_placeholder;
    [controller addScriptMessageHandler:bridge name:@"devcard"];
    // 页面据此判断是壳内还是普通浏览器，决定「导出 PDF」按钮的行为。
    [controller
        addUserScript:[[WKUserScript alloc]
                          initWithSource:@"window.__devcardShell = true;"
                           injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                        forMainFrameOnly:YES]];
    if (selftest_update_mode.length > 0) {
      [controller
          addUserScript:[[WKUserScript alloc]
                            initWithSource:@"try{localStorage.removeItem("
                                             @"'devcard.update.lastCheckAt');}catch(e){}"
                             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                          forMainFrameOnly:YES]];
    }
    config.userContentController = controller;

    WKWebView* web = [[WKWebView alloc] initWithFrame:frame configuration:config];
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    DownloadHandler* download_handler = [[DownloadHandler alloc] init];
    download_handler.window = window;
    web.navigationDelegate = download_handler;
    web.UIDelegate = download_handler;
    window.contentView = web;
    bridge.webView = web;
    bridge.window = window;
    // 关键：设 window.delegate，否则 PrintBridge 的 NSWindowDelegate 方法
    // （比如 applicationShouldTerminateAfterLastWindowClosed）不会被调用。
    window.delegate = bridge;
    [NSApp setDelegate:bridge];

    // Sparkle 2 自更新：Info.plist 里必须有 SUFeedURL + SUPublicEDKey。
    // 开发期两键为空时跳过——updaterController 是 nil，前端调 checkForUpdates 会收到提示。
    {
      NSDictionary* bundle_info = [NSBundle mainBundle].infoDictionary;
      NSString* feed_url = bundle_info[@"SUFeedURL"];
      NSString* pub_key = bundle_info[@"SUPublicEDKey"];
      if (feed_url.length > 0 && pub_key.length > 0) {
        DevCardUpdaterDelegate* del = [[DevCardUpdaterDelegate alloc] init];
        bridge.updaterDelegate = del;
        bridge.updaterController =
            [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                            updaterDelegate:del
                                                        userDriverDelegate:nil];
        NSLog(@"[sparkle] 初始化完成 feed=%@", feed_url);
      } else {
        NSLog(@"[sparkle] 跳过初始化：SUFeedURL=%@ SUPublicEDKey=%@",
              feed_url.length > 0 ? feed_url : @"(空)",
              pub_key.length > 0 ? @"(已填)" : @"(空)");
      }

      // 绑定 App 菜单「检查更新…」的 target。
      // Sparkle 启用时 target 设为 updaterController（它自己实现 checkForUpdates: + validateMenuItem:）；
      // 未启用时把菜单项隐藏，避免用户点了报错。
      if (bridge.checkUpdateMenuItem) {
        if (bridge.updaterController) {
          bridge.checkUpdateMenuItem.target = bridge.updaterController;
          bridge.checkUpdateMenuItem.hidden = NO;
        } else {
          bridge.checkUpdateMenuItem.hidden = YES;
        }
      }

      // 自动化验证：DEVCARD_SPARKLE_SELFTEST=1 时启动后自动发起一次更新检查，
      // 便于脚本核对 feed 拉取、EdDSA 校验与版本比较。
      if (bridge.updaterController &&
          [NSProcessInfo.processInfo.environment[@"DEVCARD_SPARKLE_SELFTEST"]
              isEqualToString:@"1"]) {
        NSLog(@"[sparkle] SELFTEST 准备触发 checkForUpdates");
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
              NSLog(@"[sparkle] SELFTEST 触发 checkForUpdates");
              [bridge.updaterController checkForUpdates:nil];
            });
      }
    }

    // 自动化验证：把打印视图导出成 PDF 后退出，便于脚本核对页数与文字。
    NSString* selftest_pdf =
        NSProcessInfo.processInfo.environment[@"DEVCARD_PRINT_SELFTEST_PDF"];    bridge.automaticPdfPath = selftest_pdf;
    // 负向自检：摘掉 crypto.subtle，验证「判据没跑起来」会被单独拦住。
    BOOL selftest_nosubtle =
        NSProcessInfo.processInfo.environment[@"DEVCARD_SELFTEST_NOSUBTLE"].length > 0;
    // 自动更新自检：四种失败路径（超时 / DNS / 非 JSON / 版本号非法）必须全静默。
    // 值为 live 时改打真实发布接口并保留窗口，供验收核对与截屏。
    BOOL selftest_update = selftest_update_mode.length > 0;

    NSString* page = selftest_pdf.length > 0
                         ? (selftest_nosubtle ? @"/?view=print&selftest_nosubtle=1"
                                              : @"/?view=print")
                         : @"/";
    NSString* url_string = [NSString
        stringWithFormat:@"http://127.0.0.1:%d%@", server->port(), page];
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url_string]]];

    if (selftest_update) {
      g_update_selftest_web = web;
      g_update_selftest_started = NO;
      if ([selftest_update_mode isEqualToString:@"live"]) {
        devcard_poll_update_live(80);
      } else if ([selftest_update_mode isEqualToString:@"replay"]) {
        devcard_poll_replay(60);
      } else {
        devcard_poll_update_step(60);
      }
    }

    if (selftest_pdf.length > 0) {
      __block void (^poll)(int) = nil;
      poll = ^(int attempts) {
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
                      poll(attempts - 1);
                    });
              }];
              }];
      };
      poll(50);
    }

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }
  return 0;
}
