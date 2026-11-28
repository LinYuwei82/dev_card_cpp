// Windows 桌面壳（M5 第二段）：Win32 窗口 + WebView2。
//
// 与 shell/mac/main.mm 共用同一套页面与 JS 桥协议
// （window.webkit.messageHandlers.devcard.postMessage），页面侧不改。
//
// P1–P3 拆分后本文件只保留：单实例入口、窗口过程、server 生命周期。
// 其余职责：共享状态见 shell_state.h、WebView 宿主见 webview_host.h、
// JS 桥见 bridge.h、下载见 downloads.h、打印/导出见 print_export.h。
// 验收清单见 docs/改造技术方案.md §10.9，不要在 Windows 实跑前宣称可用。

#include <windows.h>

#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "win_encoding/encoding.h"
#include "updater_win.h"

#include "devcard/server.h"
#include "downloads.h"
#include "print_export.h"
#include "shell_state.h"
#include "webview_host.h"

// 跨 TU 共享的符号集中从 devcard::win 引入，保持本文件写法紧凑。
using devcard::win::ClearDownloads;
using devcard::win::CreateWebView;
using devcard::win::DevToolsEnabled;
using devcard::win::Fatal;
using devcard::win::FinishPrint;
using devcard::win::g;
using devcard::win::kPrintFallbackTimer;
using devcard::win::kWindowClass;
using devcard::win::kWindowTitle;
using devcard::win::NavigateToApp;
using devcard::win::ResizeWebView;
using devcard::win::StartupClockBegin;
using devcard::win::StartupLog;
using devcard::win::Widen;

namespace {

// 启动速度优化：server 初始化在后台线程跑，就绪后通过自定义消息通知主线程。
// 消息载荷是堆上的 ServerStartupResult，主线程接管 unique_ptr。
constexpr UINT WM_APP_SERVER_READY = WM_APP + 7;

struct ServerStartupResult {
  std::unique_ptr<devcard::ServerHandle> server;  // null 表示启动失败
  std::string error;
};

std::unique_ptr<devcard::ServerHandle> g_server;

void ConfirmExit() {
  const int answer = MessageBoxW(
      g.hwnd,
      L"退出 DevCard 会同时停止本地服务，未保存的编辑将丢失。\n确定退出？",
      L"退出 DevCard", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
  if (answer == IDOK) DestroyWindow(g.hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                         LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      ResizeWebView();
      return 0;
    case WM_TIMER:
      if (wparam == devcard::win::kUpdateTimer) {
        devcard::win::run_update_timer(hwnd);
        return 0;
      }
      if (wparam == kPrintFallbackTimer) {
        // 兜底释放：只放锁不清 is-export 布局（无法区分「已取消」与「仍开着」，
        // 清布局可能破坏尚未出纸的打印；精确清理走 printDone）。
        FinishPrint(hwnd, false);
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case devcard::win::kUpdateResultMessage:
      // 后台 fetch 结果回流：交回引擎 onManifest（已在 UI 线程）
      if (devcard::win::handle_update_message(hwnd, lparam)) return 0;
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_APP_SERVER_READY: {
      std::unique_ptr<ServerStartupResult> res(
          reinterpret_cast<ServerStartupResult*>(lparam));
      if (!res->server) {
        // 启动失败（字体损坏/实例占用/端口冲突等）：沿用既有弹窗语义。
        g.startup_failed = true;
        Fatal(Widen(res->error));
        DestroyWindow(hwnd);
        return 0;
      }
      StartupLog("shell_start_server");
      g_server = std::move(res->server);
      g.origin =
          L"http://127.0.0.1:" + std::to_wstring(g_server->port());
      g.server_ready = true;
      // webview 可能还没建好（环境回调在消息循环里）；建好后走
      // CreateWebView 内的 server_ready 分支自行导航。
      if (g.webview) NavigateToApp();
      return 0;
    }
    case WM_CLOSE:
      if (g.server_exit_requested) {
        // server handler (auto-update install) requested exit — skip confirmation,
        // the user already triggered the update flow.
        DestroyWindow(g.hwnd);
      } else {
        ConfirmExit();
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  StartupClockBegin();
  // 从 cmd 启动或重定向时，stderr 里的中文按 UTF-8 写出，需要把控制台代码页切到 UTF-8；
  // 纯 GUI 启动（无控制台）时本调用是空操作。
  win_encoding::init_console();

  // 单实例：第二个进程不重复起服务/WebView，只把已有窗口叫到前台。
  // 用 Local\ 命名空间（同一登录会话内唯一），句柄保持到进程退出。
  HANDLE instance_mutex =
      CreateMutexW(nullptr, TRUE, L"Local\\DevCard-SingleInstance");
  if (instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    // 首实例可能还在建窗口，最多等 3 秒再放弃。
    for (int i = 0; i < 30; ++i) {
      HWND existing = FindWindowW(kWindowClass, nullptr);
      if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return 0;
      }
      Sleep(100);
    }
    return 0;
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    Fatal(L"COM 初始化失败（需要 STA 线程）。");
    return 1;
  }

  g.devtools = DevToolsEnabled();

  LPWSTR runtime_version = nullptr;
  if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr,
                                                         &runtime_version)) ||
      !runtime_version) {
    Fatal(L"未检测到 Microsoft Edge WebView2 Runtime。\n"
          L"请先安装 Evergreen Runtime（Windows 10/11 通常已自带），再启动 DevCard。");
    CoUninitialize();
    return 1;
  }
  CoTaskMemFree(runtime_version);

  devcard::ServerOptions options;
  options.data_dir = devcard::default_data_dir();
  options.port = 18080;
  g.data_dir = options.data_dir;

  // 启动速度优化：server 初始化移到后台线程，就绪后 PostMessage 到主线程。
  // 主线程立刻建窗口 + webview，先显示内联 splash，server 就绪后再导航真实页面。

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WndProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&window_class)) {
    Fatal(L"窗口类注册失败。");
    CoUninitialize();
    return 1;
  }

  RECT frame = {0, 0, 1080, 720};
  AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
  const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

  g.hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                           x < 0 ? 0 : x, y < 0 ? 0 : y, width, height, nullptr,
                           nullptr, instance, nullptr);
  if (!g.hwnd) {
    Fatal(L"主窗口创建失败。");
    CoUninitialize();
    return 1;
  }

  // 原生更新引擎：安装程序拉起后经正常关闭流程退出（停 server、释放端口）；
  // 启动 10 秒后首次后台检查，之后每 24 小时一次（节流与判定全在 core 引擎）。
  devcard::win::set_update_quit_hook([] {
    g.server_exit_requested = true;
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
  });
  devcard::win::start_update_checker(g.hwnd);

  if (!CreateWebView()) {
    CoUninitialize();
    return 1;
  }
  ShowWindow(g.hwnd, SW_SHOW);
  UpdateWindow(g.hwnd);
  StartupLog("shell_window_visible");

  // server 后台启动：就绪后 PostMessageW(WM_APP_SERVER_READY)，主线程接管。
  std::thread([options]() mutable {
    auto* result = new ServerStartupResult();
    try {
      result->server = devcard::start_server(options);
    } catch (const std::exception& error) {
      result->error = error.what();
    }
    PostMessageW(g.hwnd, WM_APP_SERVER_READY, 0,
                 reinterpret_cast<LPARAM>(result));
  }).detach();

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (g.controller) g.controller->Close();
  g.webview.Reset();
  g.controller.Reset();
  g.env.Reset();
  ClearDownloads();
  g_server.reset();
  CoUninitialize();
  return g.startup_failed ? 1 : 0;
}
