// Windows 壳 WebView2 宿主的实现（P3 从 main.cpp 拆出）。
#include "webview_host.h"

#include <windows.h>

#include <WebView2.h>
#include <wrl.h>

#include <filesystem>
#include <string>

#include "bridge.h"
#include "downloads.h"
#include "shell_state.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace devcard {
namespace win {

namespace {

// 启动 splash：纯内联 HTML，不依赖 server 与网络。
constexpr wchar_t kSplashHtml[] =
    L"<!doctype html><html><head><meta charset='utf-8'>"
    L"<style>html,body{height:100%;margin:0;"
    L"font-family:'Segoe UI','Microsoft YaHei',sans-serif;"
    L"display:flex;align-items:center;justify-content:center;"
    L"background:#f5f6f8;color:#666}"
    L".spin{width:28px;height:28px;border:3px solid #d0d5dd;"
    L"border-top-color:#2563eb;border-radius:50%;"
    L"animation:s 1s linear infinite}"
    L"@keyframes s{to{transform:rotate(360deg)}}"
    L"p{margin-top:14px;font-size:14px}</style></head>"
    L"<body><div><div class='spin' style='margin:0 auto'></div>"
    L"<p style='text-align:center'>正在启动 DevCard…</p></div></body></html>";

void ApplySettings() {
  ComPtr<ICoreWebView2Settings> settings;
  if (FAILED(g.webview->get_Settings(&settings)) || !settings) return;
  settings->put_IsStatusBarEnabled(FALSE);
  settings->put_AreDevToolsEnabled(g.devtools ? TRUE : FALSE);
  settings->put_AreDefaultContextMenusEnabled(g.devtools ? TRUE : FALSE);
  ComPtr<ICoreWebView2Settings3> settings3;
  if (SUCCEEDED(settings.As(&settings3)) && settings3) {
    // 关掉 Ctrl+P / Ctrl+S 等浏览器快捷键，打印只走页面两条通道。
    settings3->put_AreBrowserAcceleratorKeysEnabled(g.devtools ? TRUE : FALSE);
  }
}

void InjectShim() {
  // 页面按 window.webkit.messageHandlers.devcard 判断是否在壳内；
  // 这里把 WebView2 的 chrome.webview 桥映射成同一形状，页面代码不用改。
  //
  // 自动更新不再注入更新 shim：v1.4.0 分层后检测/编排全在 core UpdateEngine，
  // win 壳只提供四适配器（fetch/设置/TaskDialog/安装），提示与页面无关。
  const wchar_t* script =
      L"window.__devcardShell = true;"
      L"if (window.chrome && window.chrome.webview) {"
      L"  window.webkit = window.webkit || {};"
      L"  window.webkit.messageHandlers = window.webkit.messageHandlers || {};"
      L"  if (!window.webkit.messageHandlers.devcard) {"
      L"    window.webkit.messageHandlers.devcard = {"
      L"      postMessage: function (message) {"
      L"        window.chrome.webview.postMessage(message);"
      L"      }"
      L"    };"
      L"  }"
      L"}";
  g.webview->AddScriptToExecuteOnDocumentCreated(
      script,
      Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
          [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; })
          .Get());
}

void RegisterShellHandlers() {
  EventRegistrationToken token = {};

  g.webview->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args)
              -> HRESULT {
            PWSTR raw = nullptr;
            if (SUCCEEDED(args->get_Uri(&raw)) && raw) {
              OpenExternal(raw);
              CoTaskMemFree(raw);
            }
            args->put_Handled(TRUE);
            return S_OK;
          })
          .Get(),
      &token);

  // 页面内点外链时不让壳自己导航走，交系统浏览器。
  g.webview->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args)
              -> HRESULT {
            PWSTR raw = nullptr;
            if (FAILED(args->get_Uri(&raw)) || !raw) return S_OK;
            const std::wstring uri = raw;
            CoTaskMemFree(raw);
            if (StartsWith(uri, g.origin) || StartsWith(uri, L"about:") ||
                StartsWith(uri, L"data:") || StartsWith(uri, L"blob:") ||
                StartsWith(uri, L"devtools:")) {
              return S_OK;
            }
            args->put_Cancel(TRUE);
            OpenExternal(uri);
            return S_OK;
          })
          .Get(),
      &token);

  // F12 / Ctrl+Shift+I：Debug 或 DEVCARD_DEVTOOLS=1 时打开，否则吞掉。
  if (g.controller) {
    g.controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [](ICoreWebView2Controller*,
               ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
              COREWEBVIEW2_KEY_EVENT_KIND kind =
                  COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
              UINT key = 0;
              args->get_KeyEventKind(&kind);
              args->get_VirtualKey(&key);
              if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                  kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
                return S_OK;
              }
              const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
              const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
              const bool devtools_key =
                  key == VK_F12 ||
                  (key == L'I' && ctrl && shift);
              if (!devtools_key) return S_OK;
              if (g.devtools && g.webview) {
                g.webview->OpenDevToolsWindow();
              }
              args->put_Handled(TRUE);
              return S_OK;
            })
            .Get(),
        &token);
  }
}

}  // namespace

void ResizeWebView() {
  if (!g.controller || !g.hwnd) return;
  RECT bounds = {};
  GetClientRect(g.hwnd, &bounds);
  g.controller->put_Bounds(bounds);
}

void NavigateToApp() {
  if (!g.webview) return;
  StartupLog("webview_navigate_issued");
  const std::wstring url = g.origin + L"/";
  g.webview->Navigate(url.c_str());
}

bool CreateWebView() {
  const std::filesystem::path user_data = g.data_dir / L"webview2";
  std::error_code ec;
  std::filesystem::create_directories(user_data, ec);

  auto env_handler = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
      [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(hr) || !env) {
          Fatal(L"无法创建 WebView2 环境（" + HResultText(hr) +
                L"）。\n请确认已安装 Microsoft Edge WebView2 Runtime。");
          DestroyWindow(g.hwnd);
          return S_OK;
        }
        g.env = env;
        auto controller_handler =
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT controller_hr,
                   ICoreWebView2Controller* controller) -> HRESULT {
                  if (FAILED(controller_hr) || !controller) {
                    Fatal(L"无法创建 WebView2 控件（" +
                          HResultText(controller_hr) + L"）。");
                    DestroyWindow(g.hwnd);
                    return S_OK;
                  }
                  g.controller = controller;
                  controller->get_CoreWebView2(&g.webview);
                  controller->put_IsVisible(TRUE);
                  ResizeWebView();
                  ApplySettings();
                  RegisterBridge();
                  RegisterDownloadHandling();
                  RegisterShellHandlers();
                  InjectShim();

                  EventRegistrationToken nav_token;
                  g.webview->add_NavigationCompleted(
                      Callback<ICoreWebView2NavigationCompletedEventHandler>(
                          [](ICoreWebView2*,
                             ICoreWebView2NavigationCompletedEventArgs*)
                              -> HRESULT {
                            StartupLog("webview_navigation_completed");
                            return S_OK;
                          })
                          .Get(),
                      &nav_token);
                  // server 还在后台初始化时先显示内联 splash，就绪后
                  // WM_APP_SERVER_READY 会再导航到真实页面。
                  if (g.server_ready) {
                    NavigateToApp();
                  } else {
                    StartupLog("webview_splash_shown");
                    g.webview->NavigateToString(kSplashHtml);
                  }
                  return S_OK;
                });
        env->CreateCoreWebView2Controller(g.hwnd, controller_handler.Get());
        return S_OK;
      });

  const std::wstring user_data_dir = user_data.wstring();
  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data_dir.c_str(), nullptr, env_handler.Get());
  if (FAILED(hr)) {
    Fatal(L"无法初始化 WebView2（" + HResultText(hr) +
          L"）。\n请确认已安装 Microsoft Edge WebView2 Runtime。");
    DestroyWindow(g.hwnd);
    return false;
  }
  return true;
}

}  // namespace win
}  // namespace devcard
