// macOS 壳的共享启动设施（M1 从 main.mm 拆出）。
//
// 只放跨文件使用的启动期工具：打点、单实例锁、激活已有实例、致命错误、A4 打印设置。
// 与 Windows 壳的 shell_state 对应。MRC 所有权语义保持原样，不做任何改动。
#pragma once

#import <Cocoa/Cocoa.h>

#include <string>

// 启动耗时打点：main 入口先调 DevCardStartupClockBegin()，之后各阶段调
// DevCardStartupLog()，与 server 侧 [startup] 日志同格式。
void DevCardStartupClockBegin(void);
void DevCardStartupLog(const char* name);

// 单实例：返回持有锁的 fd（进程退出时由内核释放）；-1 打不开锁文件，
// -2 已有实例（调用方应激活它并退出）。
int DevCardAcquireSingleInstanceLock(const std::string& data_dir);
void DevCardActivateExistingInstance(void);

// 启动期致命错误：应用级模态框，返回后调用方自行退出。
void DevCardShowFatalError(const std::string& message);

// A4、零边距、100% 缩放；分页由页面里的 @page 控制。
// 返回值是 autoreleased 的 copy，调用方不要额外 release。
NSPrintInfo* DevCardA4PrintInfo(void);
