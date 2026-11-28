// macOS 壳的打印自检辅助（M4 从 main.mm 拆出）。
//
// 通过环境变量触发：
//   DEVCARD_PRINT_SELFTEST_PDF=<路径>  打开 /?view=print，点「导出 PDF」后核对产物并退出
//   DEVCARD_SELFTEST_NOSUBTLE=1        负向自检：摘掉 crypto.subtle，断言闸门能拦住
#pragma once

#import <Foundation/Foundation.h>

@class WKWebView;
@class DevCardPrintBridge;

// 自检 PDF 目标路径；未启用时返回 nil。
NSString* DevCardSelftestPdfPath(void);

// 自检启动页；未启用时为 "/"。
NSString* DevCardSelftestPagePath(void);

// 安装自检轮询（未启用时为空操作）。由窗口宿主在 present 阶段调用。
void DevCardSelftestPrepare(WKWebView* web, DevCardPrintBridge* print_bridge);

// 真实页 didFinish 后启动轮询（内部保证只启动一次，且只在 http:// 下启动）。
void DevCardSelftestKickoff(WKWebView* web);
