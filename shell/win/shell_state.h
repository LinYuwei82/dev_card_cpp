// Windows 壳的共享状态与小工具：跨翻译单元的唯一单点。
//
// P1 从 main.cpp 拆出。取舍原则：
//   - 只放被两个以上 .cpp 使用的东西；单一模块内部的辅助留在各自 .cpp。
//   - g 必须单点定义（shell_state.cpp），否则各 TU 各持一份状态。
#pragma once

#include <windows.h>

#include <WebView2.h>
#include <wrl.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace devcard {
namespace win {

// 壳的全局状态：窗口、WebView2 三件套、server 就绪标志与数据目录。
struct ShellState {
  HWND hwnd = nullptr;
  bool devtools = false;
  bool print_busy = false;
  // 由更新引擎退出钩子写入、UI 线程在 WM_CLOSE 读取，必须是原子量。
  std::atomic<bool> server_exit_requested{false};
  std::wstring origin;
  // server 后台启动完成后由 WM_APP_SERVER_READY 置位；webview 就绪时据此
  // 决定直接导航真实页面还是先显示内联 splash。
  bool server_ready = false;
  // 后台启动失败（弹窗后销毁窗口）：进程退出码保持既有的 1。
  bool startup_failed = false;
  std::filesystem::path data_dir;
  Microsoft::WRL::ComPtr<ICoreWebView2Environment> env;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview;
};

extern ShellState g;

constexpr wchar_t kWindowClass[] = L"DevCardShellWindow";
constexpr wchar_t kWindowTitle[] = L"设备管理卡";

// 启动耗时打点：wWinMain 入口先调 StartupClockBegin()，之后各阶段调
// StartupLog()，与 server 侧 [startup] 日志同格式。
void StartupClockBegin();
void StartupLog(const char* name);

// 启动期致命错误：置顶弹窗后立即退出（不留“僵尸”实例）。
void Fatal(const std::wstring& message);

// 向页面推送一条 __devcardNotify(kind, message)。
void Notify(const wchar_t* kind, const std::wstring& message);

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);
std::wstring HResultText(HRESULT hr);

bool StartsWith(const std::wstring& value, const std::wstring& prefix);
bool IsHttpUrl(const std::wstring& value);
bool DevToolsEnabled();

void OpenExternal(const std::wstring& url);
void OpenDataDir(const std::wstring& raw_path);

}  // namespace win
}  // namespace devcard
