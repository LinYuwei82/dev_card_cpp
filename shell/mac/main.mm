#import <Cocoa/Cocoa.h>
#import <PDFKit/PDFKit.h>
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

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
  if (self.onDidFinish) self.onDidFinish();
}

@end

// 打印双通道：导出 PDF 与直接打印共用同一份打印 DOM，只改变输出目标。
API_AVAILABLE(macos(11.0))
@interface PrintBridge : NSObject <WKScriptMessageHandler, NSWindowDelegate>
@property(nonatomic, assign) WKWebView* webView;
@property(nonatomic, assign) NSWindow* window;
// 自动化验证：设置后跳过保存面板，直接把 PDF 写到该路径。
@property(nonatomic, copy) NSString* automaticPdfPath;
@property(nonatomic, copy) void (^onExportResult)(BOOL ok, NSString* message);
// 打印期间持有 PDFDocument，避免任务释放后文档被回收。
@property(nonatomic, retain) PDFDocument* printDocument;
@end

@implementation PrintBridge

- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if (![message.body isKindOfClass:[NSDictionary class]]) return;
  NSDictionary* body = (NSDictionary*)message.body;
  NSString* action = body[@"action"];
  if ([action isEqualToString:@"exportPdf"]) {
    [self exportPdfNamed:body[@"filename"]];
  } else if ([action isEqualToString:@"print"]) {
    [self printCards];
  }
}

- (void)exportPdfNamed:(NSString*)suggestedName {
  NSString* name = suggestedName.length > 0 ? suggestedName : @"设备管理卡.pdf";
  // 先把页面切成 1:1、无工具栏的导出布局，再逐张 A4 截取。
  [self.webView
      evaluateJavaScript:@"window.__devcardExportLayout ? window.__devcardExportLayout() : null"
       completionHandler:^(id result, NSError* error) {
         NSDictionary* layout =
             [result isKindOfClass:[NSDictionary class]] ? result : nil;
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
    WKUserContentController* controller = [[WKUserContentController alloc] init];
    PrintBridge* bridge = [[PrintBridge alloc] init];
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
    web.navigationDelegate = download_handler;
    web.UIDelegate = download_handler;
    window.contentView = web;
    bridge.webView = web;
    bridge.window = window;

    // 自动化验证：把打印视图导出成 PDF 后退出，便于脚本核对页数与文字。
    NSString* selftest_pdf =
        NSProcessInfo.processInfo.environment[@"DEVCARD_PRINT_SELFTEST_PDF"];    bridge.automaticPdfPath = selftest_pdf;

    NSString* page = selftest_pdf.length > 0 ? @"/?view=print" : @"/";
    NSString* url_string = [NSString
        stringWithFormat:@"http://127.0.0.1:%d%@", server->port(), page];
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url_string]]];

    if (selftest_pdf.length > 0) {
      __block void (^poll)(int) = nil;
      poll = ^(int attempts) {
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
      };
      poll(50);
    }

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }
  return 0;
}
