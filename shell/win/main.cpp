// Windows 桌面壳（M5 第二段）：Win32 窗口 + WebView2。
//
// 与 shell/mac/main.mm 共用同一套页面与 JS 桥协议
// （window.webkit.messageHandlers.devcard.postMessage），页面侧不改。
//
// 状态：代码已写，尚未在 Windows 上编译与实跑。验收清单见
// docs/改造技术方案.md §10.9，不要在 Windows 实跑前宣称可用。

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <winspool.h>
#include <commctrl.h>
#include <wrl.h>

#include <WebView2.h>

#include "win_encoding/encoding.h"
#include "devcard/update_manifest.h"
#include "updater_win.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "devcard/server.h"
#include "print_export.h"
#include "shell_state.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// P1：跨 TU 共享的状态与工具已迁到 shell_state.h/.cpp，这里显式引入，
// 保持本文件其余代码的写法不变。
using devcard::win::DevToolsEnabled;
using devcard::win::ExportPdf;
using devcard::win::Fatal;
using devcard::win::FinishPrint;
using devcard::win::g;
using devcard::win::HResultText;
using devcard::win::IsHttpUrl;
using devcard::win::kPrintFallbackTimer;
using devcard::win::kWindowClass;
using devcard::win::kWindowTitle;
using devcard::win::Narrow;
using devcard::win::Notify;
using devcard::win::OpenDataDir;
using devcard::win::OpenExternal;
using devcard::win::ShowPrintDialog;
using devcard::win::StartupClockBegin;
using devcard::win::StartupLog;
using devcard::win::StartsWith;
using devcard::win::Widen;

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

std::unique_ptr<devcard::ServerHandle> g_server;

std::filesystem::path DownloadsDir() {
  PWSTR raw = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT,
                                     nullptr, &raw))) {
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
  }
  wchar_t profile[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  if (length > 0) return std::filesystem::path(profile) / L"Downloads";
  return std::filesystem::current_path();
}

// 同名文件不覆盖：追加 (1)、(2) … 与浏览器下载行为一致。
std::filesystem::path UniquePath(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return path;
  const std::filesystem::path dir = path.parent_path();
  const std::wstring stem = path.stem().wstring();
  const std::wstring ext = path.extension().wstring();
  for (int i = 1; i < 1000; ++i) {
    std::filesystem::path candidate =
        dir / (stem + L" (" + std::to_wstring(i) + L")" + ext);
    if (!std::filesystem::exists(candidate, ec)) return candidate;
  }
  return path;
}

// ---- 「关于」对话框（帮助菜单）----
// 版本/构建号来自编译期注入（VERSION.txt + tag 序号），与更新链路清单版本
// 同源；链接用 SYSLINK 控件，打开 OSS 更新记录/手册与 GitHub 发布页。
constexpr wchar_t kAboutClass[] = L"DevCardAboutDialog";
constexpr int IDC_ABOUT_OK = 101;
constexpr int IDC_ABOUT_NOTES = 102;
constexpr int IDC_ABOUT_MANUAL = 103;
constexpr int IDC_ABOUT_RELEASES = 104;

std::wstring AboutLinkUrl(int id, const std::wstring& version) {
  if (id == IDC_ABOUT_NOTES)
    return L"https://dl.xiaotu.online/devcard/release-notes-" + version +
           L".html";
  if (id == IDC_ABOUT_MANUAL)
    return L"https://dl.xiaotu.online/devcard/manual.pdf";
  if (id == IDC_ABOUT_RELEASES)
    return L"https://github.com/LinYuwei82/dev_card_cpp/releases";
  return L"";
}

LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      const HINSTANCE inst =
          reinterpret_cast<LPCREATESTRUCTW>(lp)->hInstance;
      const UINT dpi = GetDpiForWindow(hwnd);
      auto S = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
      NONCLIENTMETRICSW ncm = {};
      ncm.cbSize = sizeof(ncm);
      SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
      HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
      ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
      ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 4;
      HFONT title_font = CreateFontIndirectW(&ncm.lfMessageFont);
      SetPropW(hwnd, L"font", font);
      SetPropW(hwnd, L"titlefont", title_font);

      const std::wstring ver = Widen(devcard::current_product_version());
      const uint64_t build = devcard::compiled_build_number();
      std::wstring ver_line = L"版本 v" + ver;
      if (build > 0) ver_line += L"（构建 " + std::to_wstring(build) + L"）";
      // 属性里存堆拷贝（ver 局部量在 WM_CREATE 结束后失效），WM_DESTROY 释放。
      wchar_t* ver_copy = new wchar_t[ver.size() + 1];
      ver.copy(ver_copy, ver.size());
      ver_copy[ver.size()] = 0;
      SetPropW(hwnd, L"ver", reinterpret_cast<HANDLE>(ver_copy));

      // app.rc 里 IDI_ICON1 为未定义标识符，资源编译器按 1 处理。
      HICON icon = static_cast<HICON>(LoadImageW(
          inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), LR_DEFAULTCOLOR));
      if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
      HWND icon_c = CreateWindowExW(0, L"STATIC", nullptr,
                                    WS_CHILD | WS_VISIBLE | SS_ICON, S(20),
                                    S(18), S(48), S(48), hwnd, nullptr, inst,
                                    nullptr);
      SendMessageW(icon_c, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);

      auto add_label = [&](const std::wstring& text, int x, int y, HFONT f) {
        HWND c = CreateWindowExW(0, L"STATIC", text.c_str(),
                                 WS_CHILD | WS_VISIBLE | SS_LEFT, S(x), S(y),
                                 S(360), S(22), hwnd, nullptr, inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
      };
      add_label(L"DevCard 设备管理卡", 84, 20, title_font);
      add_label(ver_line, 84, 50, font);
      add_label(L"物业设备台账与设备管理卡打印工具", 20, 92, font);
      add_label(L"© 2026 小兔在线", 20, 116, font);

      auto add_link = [&](const std::wstring& label, int id, int y) {
        const std::wstring t = L"<a href=\"" + AboutLinkUrl(id, ver) + L"\">" +
                               label + L"</a>";
        HWND c = CreateWindowExW(
            0, WC_LINK, t.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(20),
            S(y), S(220), S(20), hwnd,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(static_cast<int>(id))),
            inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      };
      add_link(L"更新记录", IDC_ABOUT_NOTES, 148);
      add_link(L"用户手册", IDC_ABOUT_MANUAL, 172);
      add_link(L"发布页", IDC_ABOUT_RELEASES, 196);

      HWND ok = CreateWindowExW(
          0, L"BUTTON", L"确定",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, S(300),
          S(222), S(80), S(26), hwnd,
          reinterpret_cast<HMENU>(
              static_cast<UINT_PTR>(static_cast<int>(IDC_ABOUT_OK))),
          inst, nullptr);
      SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      SetFocus(ok);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDC_ABOUT_OK) DestroyWindow(hwnd);
      return 0;
    case WM_NOTIFY: {
      const LPNMHDR nm = reinterpret_cast<LPNMHDR>(lp);
      if ((nm->code == NM_CLICK || nm->code == NM_RETURN) &&
          nm->hwndFrom == GetDlgItem(hwnd, static_cast<int>(nm->idFrom))) {
        const LPWSTR ver = reinterpret_cast<LPWSTR>(GetPropW(hwnd, L"ver"));
        const std::wstring url =
            AboutLinkUrl(static_cast<int>(nm->idFrom), ver ? ver : L"");
        if (!url.empty()) OpenExternal(url);
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) DestroyWindow(hwnd);
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY: {
      HFONT f = static_cast<HFONT>(RemovePropW(hwnd, L"font"));
      HFONT tf = static_cast<HFONT>(RemovePropW(hwnd, L"titlefont"));
      if (f) DeleteObject(f);
      if (tf) DeleteObject(tf);
      delete[] reinterpret_cast<LPWSTR>(RemovePropW(hwnd, L"ver"));
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowAboutDialog(HWND parent) {
  static bool inited = false;
  if (!inited) {
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LINK_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AboutWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kAboutClass;
    RegisterClassExW(&wc);
    inited = true;
  }
  const UINT dpi = GetDpiForWindow(parent);
  const int cw = MulDiv(400, static_cast<int>(dpi), 96);
  const int ch = MulDiv(260, static_cast<int>(dpi), 96);
  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  RECT cr = {0, 0, cw, ch};
  AdjustWindowRectEx(&cr, style, FALSE, WS_EX_DLGMODALFRAME);
  RECT pr = {};
  GetWindowRect(parent, &pr);
  const int x = pr.left + ((pr.right - pr.left) - (cr.right - cr.left)) / 2;
  const int y = pr.top + ((pr.bottom - pr.top) - (cr.bottom - cr.top)) / 2;
  HWND dlg = CreateWindowExW(
      WS_EX_DLGMODALFRAME, kAboutClass, L"关于 DevCard", style, x, y,
      cr.right - cr.left, cr.bottom - cr.top, parent, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (!dlg) return;
  EnableWindow(parent, FALSE);
  ShowWindow(dlg, SW_SHOW);
  MSG m;
  while (IsWindow(dlg) && GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(dlg, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  EnableWindow(parent, TRUE);
  SetForegroundWindow(parent);
}

// ---------- 下载 ----------

struct DownloadWatch {
  ComPtr<ICoreWebView2DownloadOperation> operation;
  EventRegistrationToken token = {};
  bool finished = false;
};

std::vector<std::shared_ptr<DownloadWatch>> g_downloads;

void WatchDownload(ComPtr<ICoreWebView2DownloadOperation> operation) {
  if (!operation) return;
  auto watch = std::make_shared<DownloadWatch>();
  watch->operation = operation;
  auto handler = Callback<ICoreWebView2StateChangedEventHandler>(
      [watch](ICoreWebView2DownloadOperation* sender,
              IUnknown*) -> HRESULT {
        if (!sender) return S_OK;
        COREWEBVIEW2_DOWNLOAD_STATE state =
            COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
        sender->get_State(&state);
        if (state == COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS) return S_OK;
        if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
          PWSTR raw = nullptr;
          sender->get_ResultFilePath(&raw);
          const std::wstring path = raw ? raw : L"";
          if (raw) CoTaskMemFree(raw);
          Notify(nullptr, path.empty() ? std::wstring(L"下载完成")
                                       : L"已下载到：" + path);
        } else {
          Notify(L"error", L"下载中断，请重试");
        }
        if (!watch->finished) {
          watch->finished = true;
          sender->remove_StateChanged(watch->token);
          // 下载结束即从列表摘除：g_downloads 原先只增不减，
          // 本回调已按值捕获 watch，摘除后仍存活到返回，安全。
          for (std::size_t i = 0; i < g_downloads.size(); ++i) {
            if (g_downloads[i].get() == watch.get()) {
              g_downloads.erase(g_downloads.begin() + i);
              break;
            }
          }
        }
        return S_OK;
      });
  if (SUCCEEDED(operation->add_StateChanged(handler.Get(), &watch->token))) {
    g_downloads.push_back(watch);
  }
}

void HandleDownloadStarting(ICoreWebView2DownloadStartingEventArgs* args) {
  if (!args) return;
  ComPtr<ICoreWebView2DownloadOperation> operation;
  args->get_DownloadOperation(&operation);

  PWSTR raw = nullptr;
  args->get_ResultFilePath(&raw);
  std::filesystem::path target =
      raw ? std::filesystem::path(raw) : (DownloadsDir() / L"download.bin");
  if (raw) CoTaskMemFree(raw);
  target = UniquePath(target);

  args->put_ResultFilePath(target.c_str());
  // 由壳接管下载面板；文件名与目标目录由上面决定。
  args->put_Handled(TRUE);
  WatchDownload(operation);
}

// ---------- JS 桥 ----------

void HandleBridgeMessage(const std::wstring& json_text) {
  try {
    const nlohmann::json message = nlohmann::json::parse(Narrow(json_text));
    if (!message.is_object()) return;
    const std::string action = message.value("action", std::string());
    if (action == "exportPdf") {
      ExportPdf(message.value("filename", std::string("设备管理卡.pdf")));
    } else if (action == "print") {
      ShowPrintDialog();
    } else if (action == "printDone") {
      // 页面 afterprint 已触发：打印对话框关闭或出纸完成，释放打印互斥并收布局。
      FinishPrint(g.hwnd, true);
    } else if (action == "openDataDir") {
      OpenDataDir(Widen(message.value("path", std::string())));
    } else if (action == "showAbout") {
      // 状态栏版本号点击：弹原生「关于 DevCard」面板。
      ShowAboutDialog(g.hwnd);
    } else if (action == "checkForUpdates") {
      // 状态栏「检查更新」按钮：原生引擎立即检查（与 macOS 同一入口形态）。
      devcard::win::check_for_updates_now(g.hwnd);
    } else if (action == "openExternal") {
      OpenExternal(Widen(message.value("url", std::string())));
    } else if (action == "quit") {
      // 【回归 Bug 修复｜请勿改动】页面顶栏「退出」在桌面版等于退出应用。
      // 走 WM_CLOSE 复用关窗确认框与停机流程；删掉这条分支，
      // 前端就会落回注销分支停在登录页（历史反馈已出现两次）。
      PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    }
  } catch (const std::exception&) {
    // 页面只会发我们自己定义的 JSON；解析失败时忽略即可。
  }
}

void RegisterHandlers() {
  EventRegistrationToken token = {};

  g.webview->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args)
              -> HRESULT {
            PWSTR raw = nullptr;
            if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
              HandleBridgeMessage(raw);
              CoTaskMemFree(raw);
            }
            return S_OK;
          })
          .Get(),
      &token);

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

  ComPtr<ICoreWebView2_4> webview4;
  if (SUCCEEDED(g.webview.As(&webview4)) && webview4) {
    webview4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args)
                -> HRESULT {
              HandleDownloadStarting(args);
              return S_OK;
            })
            .Get(),
        &token);
  }

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

void ResizeWebView() {
  if (!g.controller || !g.hwnd) return;
  RECT bounds = {};
  GetClientRect(g.hwnd, &bounds);
  g.controller->put_Bounds(bounds);
}

// ---------- 窗口 ----------

// 启动速度优化：server 初始化在后台线程跑，就绪后通过自定义消息通知主线程。
// 消息载荷是堆上的 ServerStartupResult，主线程接管 unique_ptr。
constexpr UINT WM_APP_SERVER_READY = WM_APP + 7;

struct ServerStartupResult {
  std::unique_ptr<devcard::ServerHandle> server;  // null 表示启动失败
  std::string error;
};

void NavigateToApp() {
  if (!g.webview) return;
  StartupLog("webview_navigate_issued");
  const std::wstring url = g.origin + L"/";
  g.webview->Navigate(url.c_str());
}

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
                  RegisterHandlers();
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
  g_downloads.clear();
  g_server.reset();
  CoUninitialize();
  return g.startup_failed ? 1 : 0;
}
