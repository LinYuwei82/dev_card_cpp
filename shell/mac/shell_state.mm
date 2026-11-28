// macOS 壳共享启动设施的实现（M1 从 main.mm 拆出）。
#include "shell_state.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <system_error>

namespace {

// 启动耗时打点基准，从 main 入口起算。
std::chrono::steady_clock::time_point g_startup_t0;

}  // namespace

void DevCardStartupClockBegin(void) {
  g_startup_t0 = std::chrono::steady_clock::now();
}

void DevCardStartupLog(const char* name) {
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - g_startup_t0)
                        .count();
  NSLog(@"[startup] %s = %.1fms(cum)", name, ms);
}

// 单实例：锁文件放在数据目录，第二个进程拿不到锁就激活已有窗口后退出。
// 用独立的 shell.lock，避免和服务端自己的实例锁互相影响。
int DevCardAcquireSingleInstanceLock(const std::string& data_dir) {
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  const std::string lock_path = data_dir + "/shell.lock";
  const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd < 0) return -1;
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return -2;
  }
  return fd;  // 进程存活期间一直持有，退出时由内核释放
}

void DevCardActivateExistingInstance(void) {
  NSArray<NSRunningApplication*>* apps = [NSRunningApplication
      runningApplicationsWithBundleIdentifier:@"com.anye.devcard.new"];
  const pid_t self = NSProcessInfo.processInfo.processIdentifier;
  for (NSRunningApplication* app in apps) {
    if (app.processIdentifier == self) continue;
    [app activateWithOptions:NSApplicationActivateAllWindows];
    break;
  }
}

void DevCardShowFatalError(const std::string& message) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"DevCard 启动失败";
  alert.informativeText = [NSString stringWithUTF8String:message.c_str()];
  [alert addButtonWithTitle:@"退出"];
  [alert runModal];
  [alert release];
}

NSPrintInfo* DevCardA4PrintInfo(void) {
  NSPrintInfo* info = [NSPrintInfo.sharedPrintInfo copy];
  info.paperSize = NSMakeSize(595.28, 841.89);
  info.orientation = NSPaperOrientationPortrait;
  info.topMargin = 0;
  info.bottomMargin = 0;
  info.leftMargin = 0;
  info.rightMargin = 0;
  info.horizontallyCentered = NO;
  info.verticallyCentered = NO;
  // 调用方都是同步传给 printOperationForPrintInfo:（内部自行 retain），
  // 这里 autorelease 掉 copy 的所有权，避免每次打印/导出泄漏一份。
  return [info autorelease];
}
