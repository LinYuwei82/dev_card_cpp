// Windows 壳共享状态与小工具的实现（P1 从 main.cpp 拆出）。
//
// 这里的内容原先都在 main.cpp 的匿名 namespace 里，被多个功能块反复使用：
// 全局状态、启动打点、致命错误、页面通知、宽窄字符串转换、外链/数据目录打开。
#include "shell_state.h"

#include <shellapi.h>

#include <chrono>
#include <cwchar>
#include <cwctype>
#include <iostream>

#include <nlohmann/json.hpp>

using Microsoft::WRL::Callback;

namespace devcard {
namespace win {

ShellState g;

namespace {

std::chrono::steady_clock::time_point g_startup_t0;

}  // namespace

void StartupClockBegin() {
  g_startup_t0 = std::chrono::steady_clock::now();
}

void StartupLog(const char* name) {
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - g_startup_t0)
                        .count();
  // 用 cerr+endl 而非 fprintf：MSVC 重定向到文件时 fprintf(stderr) 可能滞留
  // 缓冲，验收脚本 taskkill 强杀会丢尾部日志。
  std::cerr << "[startup] " << name << " = " << ms << "ms(cum)" << std::endl;
}

void Fatal(const std::wstring& message) {
  // MB_TOPMOST | MB_SETFOREGROUND 让 MessageBox 抢过所有窗口置顶，
  // 避免被其他进程的窗口挡住导致用户看不到错误。
  // MessageBox 返回后（用户点"确定"）立即 ExitProcess(1)，
  // 不留一个进程存活但服务器/窗口都没起来的"僵尸"实例。
  // ExitProcess 跳过 C++ 栈展开，但 Windows 会自动关闭所有句柄——
  // InstanceGuard 的 LockFileEx、DB mutex、socket 都会被释放。
  MessageBoxW(nullptr, message.c_str(), L"DevCard 启动失败",
              MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
  ExitProcess(1);
}

void Notify(const wchar_t* kind, const std::wstring& message) {
  if (!g.webview) return;
  nlohmann::json kind_json =
      kind ? nlohmann::json(Narrow(kind)) : nlohmann::json(nullptr);
  const std::string js =
      "window.__devcardNotify && window.__devcardNotify(" + kind_json.dump() +
      "," + nlohmann::json(Narrow(message)).dump() + ");";
  const std::wstring wide_js = Widen(js);
  auto handler = Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
      [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
  g.webview->ExecuteScript(wide_js.c_str(), handler.Get());
}

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                       static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                       static_cast<int>(wide.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring HResultText(HRESULT hr) {
  wchar_t buffer[16] = {};
  swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
  return buffer;
}

bool StartsWith(const std::wstring& value, const std::wstring& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool IsHttpUrl(const std::wstring& value) {
  const std::wstring lower = [&value] {
    std::wstring s = value;
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
  }();
  return StartsWith(lower, L"https://") || StartsWith(lower, L"http://");
}

bool DevToolsEnabled() {
  wchar_t buffer[16] = {};
  const DWORD length = GetEnvironmentVariableW(L"DEVCARD_DEVTOOLS", buffer, 16);
  if (length > 0) {
    const wchar_t c = buffer[0];
    return c == L'1' || c == L't' || c == L'T' || c == L'y' || c == L'Y';
  }
#if !defined(NDEBUG)
  return true;
#else
  return false;
#endif
}

void OpenExternal(const std::wstring& url) {
  if (!IsHttpUrl(url)) {
    Notify(L"error", L"链接地址无效");
    return;
  }
  const HINSTANCE result = ShellExecuteW(g.hwnd, L"open", url.c_str(), nullptr,
                                         nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    Notify(L"error", L"无法打开链接：" + url);
  }
}

void OpenDataDir(const std::wstring& raw_path) {
  if (raw_path.empty()) {
    Notify(L"error", L"数据目录为空");
    return;
  }
  const std::filesystem::path path(raw_path);
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    Notify(L"error", L"数据目录不存在：" + path.wstring());
    return;
  }
  const HINSTANCE result =
      ShellExecuteW(g.hwnd, L"open", path.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    Notify(L"error", L"无法打开数据目录：" + path.wstring());
  }
}

}  // namespace win
}  // namespace devcard
