// macOS 壳的打印双通道（M2 从 main.mm 拆出）。
#import "print_export.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "shell_state.h"

@interface DevCardPrintBridge ()
- (void)buildPdfFromRects:(NSArray*)rects
               completion:(void (^)(NSData* pdf, NSInteger pages, NSString* error))completion;
- (void)captureRects:(NSArray*)rects
               index:(NSInteger)index
           collected:(NSMutableArray*)collected
          completion:(void (^)(NSData* pdf, NSInteger pages, NSString* error))completion;
- (NSData*)pdfDataFromPages:(NSArray*)pages error:(NSString**)error;
- (void)finishExport:(BOOL)ok message:(NSString*)message;
@end

@implementation DevCardPrintBridge

- (void)exportPdfNamed:(NSString*)suggestedName {
  NSString* name = suggestedName.length > 0 ? suggestedName : @"设备管理卡.pdf";
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
  DevCardNotify(self.webView, message, ok ? nil : @"error");
  if (self.onExportResult) self.onExportResult(ok, message);
}

- (void)printCards {
  // 没装打印机时打印面板会一直等不到纸张尺寸，先给出明确提示。
  NSString* printer_name = [NSPrintInfo.sharedPrintInfo.printer.name
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  if (printer_name.length == 0) {
    DevCardNotify(self.webView,
                  @"系统未添加打印机，请先在「系统设置 › 打印机与扫描仪」中添加，或改用导出 PDF",
                  @"error");
    return;
  }

  // 直接打印复用导出 PDF 的同一份 A4 数据，避免 WebKit 打印会话的版式差异。
  [self.webView
      evaluateJavaScript:@"window.__devcardExportLayout ? window.__devcardExportLayout() : null"
       completionHandler:^(id result, NSError* error) {
         NSDictionary* layout =
             [result isKindOfClass:[NSDictionary class]] ? result : nil;
         if ([layout[@"error"] length] > 0) {
           DevCardNotify(self.webView, layout[@"error"], @"error");
           return;
         }
         NSArray* rects = layout[@"rects"];
         if (rects.count == 0) {
           DevCardNotify(self.webView, @"没有可打印的卡片", @"error");
           return;
         }
         [self buildPdfFromRects:rects
                      completion:^(NSData* pdf, NSInteger pages, NSString* error) {
                        if (pdf.length == 0) {
                          DevCardNotify(self.webView, error ?: @"无法生成打印数据", @"error");
                          return;
                        }
                        PDFDocument* document = [[PDFDocument alloc] initWithData:pdf];
                        if (!document) {
                          DevCardNotify(self.webView, @"无法生成打印数据", @"error");
                          return;
                        }
                        NSPrintOperation* operation =
                            [document printOperationForPrintInfo:DevCardA4PrintInfo()
                                                    scalingMode:kPDFPrintPageScaleNone
                                                     autoRotate:NO];
                        if (!operation) {
                          DevCardNotify(self.webView, @"无法创建打印任务", @"error");
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
  if (!success) DevCardNotify(self.webView, @"打印已取消或失败", nil);
}

@end
