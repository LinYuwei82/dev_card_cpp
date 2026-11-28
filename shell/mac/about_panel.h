// macOS 壳的「关于 DevCard」面板（M1 从 main.mm 拆出）。
//
// 版本/构建号来自编译期注入（VERSION.txt + tag 序号），与更新链路清单版本
// 同源。菜单项 target 与页面状态栏版本号点击（桥 showAbout）共用同一实例。
#pragma once

#import <Cocoa/Cocoa.h>

@interface DevCardAboutHelper : NSObject
- (void)showAbout:(id)sender;
@end

// 常驻单例（进程存活期间一直有效），替代原来的 main 内 static 变量。
DevCardAboutHelper* DevCardSharedAboutHandler(void);
