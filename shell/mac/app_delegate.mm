// macOS 壳的应用/窗口生命周期代理（M3 从 main.mm 拆出）。
#import "app_delegate.h"

@implementation DevCardAppDelegate

// 点窗口红叉（关闭按钮）时，关闭最后一个窗口 = 终止 App。
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}

// 关闭主窗口时也直接 terminate，不等 applicationShouldTerminateAfterLastWindowClosed:
// 的下一轮事件循环——避免窗口已消失但 Dock 图标还闪一下。
- (BOOL)windowShouldClose:(id)sender {
  [NSApp terminate:nil];
  return YES;
}

@end
