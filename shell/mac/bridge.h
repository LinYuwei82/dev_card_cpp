// macOS 壳的 JS 桥（M3 从 main.mm 拆出）。
//
// 页面通过 window.webkit.messageHandlers.devcard.postMessage 发来的全部动作
// 都在这里分发：导出/打印转 print_export，文件保存/落点选择留在本类，
// 更新检查转 updater，关于面板转 about_panel。
#pragma once

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@class DevCardPrintBridge;
@class DevCardUpdaterBridge;
@class DevCardAboutHelper;

NS_ASSUME_NONNULL_BEGIN

API_AVAILABLE(macos(11.0))
@interface DevCardBridge : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) WKWebView* webView;
@property(nonatomic, assign) NSWindow* window;
// 导出/打印通道（M2 拆到 print_export.mm，桥只做转发）。
@property(nonatomic, assign) DevCardPrintBridge* printBridge;
// Sparkle 自更新桥（桥内部持有 controller + delegate）。
@property(nonatomic, strong) DevCardUpdaterBridge* updaterBridge;
// 「关于 DevCard」面板处理器（状态栏版本号点击走 showAbout 时复用）。
@property(nonatomic, assign, nullable) DevCardAboutHelper* aboutHandler;
@end

NS_ASSUME_NONNULL_END
