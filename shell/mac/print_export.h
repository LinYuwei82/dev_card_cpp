// macOS 壳的打印双通道（M2 从 main.mm 拆出）。
//
// 导出 PDF 与直接打印共用同一份打印 DOM：先把页面切成 1:1 导出布局，再逐张
// A4 截取合成 PDF。两者只有输出目标不同（写文件 vs NSPrintOperation）。
#pragma once

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <Quartz/Quartz.h>

@interface DevCardPrintBridge : NSObject
@property(nonatomic, assign) WKWebView* webView;
@property(nonatomic, assign) NSWindow* window;
// 自动化验证：设置后跳过保存面板，直接把 PDF 写到该路径。
@property(nonatomic, copy) NSString* automaticPdfPath;
@property(nonatomic, copy) void (^onExportResult)(BOOL ok, NSString* message);
// 打印期间持有 PDFDocument，避免任务释放后文档被回收。
@property(nonatomic, retain) PDFDocument* printDocument;

- (void)exportPdfNamed:(NSString*)suggestedName;
- (void)printCards;
@end
