// macOS 壳的应用/窗口生命周期代理（M3 从 main.mm 拆出）。
//
// 关闭红叉即退出应用：没有 applicationShouldTerminateAfterLastWindowClosed
// 时，红叉只关窗口但 NSApp 继续跑（留在 Dock 里），用户必须 Cmd+Q 才能退出。
#pragma once

#import <Cocoa/Cocoa.h>

@interface DevCardAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
