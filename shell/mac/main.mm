// macOS 桌面壳入口。
//
// M1–M4 拆分后本文件只保留：单实例入口、server 生命周期、事件循环。
// 其余职责：启动期共享设施见 shell_state.h、窗口/WebView 装配见
// window_host.h、JS 桥见 bridge.h、打印自检见 print_selftest.h。
#import <Cocoa/Cocoa.h>

#include <cerrno>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "devcard/server.h"

#import "shell_state.h"
#import "window_host.h"

int main(int argc, const char* argv[]) {
  DevCardStartupClockBegin();
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    devcard::ServerOptions options;
    options.data_dir = devcard::default_data_dir();
    options.port = 18080;

    const int instance_lock = DevCardAcquireSingleInstanceLock(options.data_dir);
    if (instance_lock == -2) {
      // 已有实例：把它叫到前台，本进程直接退出，不弹错误框。
      DevCardActivateExistingInstance();
      return 0;
    }
    if (instance_lock < 0) {
      // 锁文件打不开（目录权限等）：壳层单实例防护失效，继续启动并依赖
      // server 层的实例锁兜底，但必须留下日志，不能让失效无声无息。
      NSLog(@"[devcard] 警告：无法创建 shell.lock，壳层单实例防护跳过（errno=%d）",
            errno);
    }

    // 启动速度优化：server 初始化（含字体校验/DB 迁移）移到后台线程；
    // 主线程立刻建窗口并显示内联 splash，就绪后再导航到真实页面。
    // 句柄用 shared_ptr 套壳：后台线程产出、主线程块里接管，生命周期仍随 main。
    auto server_holder =
        std::make_shared<std::unique_ptr<devcard::ServerHandle>>();

    DevCardWindowHost* host = [[DevCardWindowHost alloc] init];
    [host present];

    std::thread([options, server_holder, host]() {
      auto result = std::make_shared<std::unique_ptr<devcard::ServerHandle>>();
      std::string startup_error;
      try {
        *result = devcard::start_server(options);
      } catch (const std::exception& error) {
        startup_error = error.what();
      }
      DevCardStartupLog("shell_start_server");
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!*result) {
          // 启动失败（字体损坏/实例占用/端口冲突等）：沿用既有弹窗语义。
          DevCardShowFatalError(startup_error);
          [NSApp terminate:nil];
          return;
        }
        *server_holder = std::move(*result);
        [host navigateToPageWithPort:(*server_holder)->port()];
      });
    }).detach();

    [NSApp run];
  }
  return 0;
}
