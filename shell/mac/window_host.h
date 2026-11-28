// macOS 壳的窗口/WebView 宿主（M4 从 main.mm 拆出）。
//
// 负责菜单、主窗口、WKWebView、JS 桥、下载/原生面板宿主与 splash 的装配。
// server 生命周期仍留在 main：启动就绪后由 main 调用 navigateToPageWithPort:
// 导航到真实页面。
#pragma once

#import <Cocoa/Cocoa.h>

@interface DevCardWindowHost : NSObject
// 建菜单/窗口/WebView/桥并显示内联 splash。
- (void)present;
// server 就绪后导航到 http://127.0.0.1:<port><page>。
// page 由打印自检环境变量决定（普通启动为 "/"）。
- (void)navigateToPageWithPort:(int)port;
@end
