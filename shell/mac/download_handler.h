// macOS 壳的下载与页面原生面板宿主（M2 从 main.mm 拆出）。
//
// WKWebView 默认不处理 <a download> / Content-Disposition 下载，导出会静默失败；
// 不实现 JS 面板回调时 confirm/alert 会被 WebKit 当作取消（删除账号、恢复备份
// 就会「点了没反应」）。这两类都集中在这里接管。
#pragma once

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

API_AVAILABLE(macos(11.0))
@interface DevCardDownloadHandler
    : NSObject <WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate>
@property(nonatomic, copy) void (^onDidFinish)(void);
// 文件选择面板与确认框挂在窗口上（sheet），没有窗口时退回应用级模态。
@property(nonatomic, assign) NSWindow* window;
@end
