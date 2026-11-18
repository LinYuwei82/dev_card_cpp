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
#include <wrl.h>

#include <WebView2.h>

#include <atomic>
#include <cstdio>
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

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"DevCardShellWindow";
constexpr wchar_t kWindowTitle[] = L"设备管理卡";

// 菜单栏命令 ID（帮助菜单）
constexpr int IDM_HELP_CHECKUPDATE = 1001;
constexpr int IDM_HELP_ABOUT = 1002;

struct ShellState {
  HWND hwnd = nullptr;
  bool devtools = false;
  bool print_busy = false;
  // 由 server_exit_watcher 线程写入、UI 线程在 WM_CLOSE 读取，必须是原子量。
  std::atomic<bool> server_exit_requested{false};
  // 退出流程的停机信号：server_exit_watcher 靠它跳出轮询，
  // 否则正常关窗时 join() 会永久阻塞（进程活着、18080 端口不释放）。
  std::atomic<bool> shutdown{false};
  std::wstring origin;
  std::filesystem::path data_dir;
  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
};

ShellState g;
std::unique_ptr<devcard::ServerHandle> g_server;

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

// 从壳层菜单栏触发前端检查更新。前端必须已加载（window.devcard.checkUpdate）。
void TriggerFrontendCheckUpdate() {
  if (!g.webview) return;
  const std::wstring js =
      L"window.devcard && window.devcard.checkUpdate && window.devcard.checkUpdate();";
  auto handler = Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
      [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
  g.webview->ExecuteScript(js.c_str(), handler.Get());
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

void CreateA4PrintSettings(ComPtr<ICoreWebView2PrintSettings>* settings) {
  ComPtr<ICoreWebView2Environment6> env6;
  if (FAILED(g.env.As(&env6)) || !env6) return;
  if (FAILED(env6->CreatePrintSettings(settings->ReleaseAndGetAddressOf())) ||
      settings->Get() == nullptr) {
    return;
  }
  // WebView2 的 PrintSettings 单位是英寸；A4 = 8.27 × 11.69 in。
  (*settings)->put_Orientation(COREWEBVIEW2_PRINT_ORIENTATION_PORTRAIT);
  (*settings)->put_PageWidth(8.27);
  (*settings)->put_PageHeight(11.69);
  (*settings)->put_MarginTop(0.0);
  (*settings)->put_MarginBottom(0.0);
  (*settings)->put_MarginLeft(0.0);
  (*settings)->put_MarginRight(0.0);
  (*settings)->put_ScaleFactor(1.0);
  (*settings)->put_ShouldPrintBackgrounds(TRUE);
  (*settings)->put_ShouldPrintHeaderAndFooter(FALSE);
}

// 文件名兜底：SetFileName 收下非法字符后，用户要等点到「保存」才被系统拒绝，
// 现象就是「PDF 存不下来」且不说是哪个字符。这里先按 Windows 规则收口：
// 路径分隔符与通配符换掉、去掉结尾的点与空格、保留名加前缀。
std::wstring SanitizeLeaf(std::wstring leaf) {
  static const wchar_t kIllegal[] = L"\\/:*?\"<>|";
  for (wchar_t& c : leaf) {
    if (c < 0x20) {
      c = L'_';
      continue;
    }
    for (const wchar_t* p = kIllegal; *p; ++p) {
      if (c == *p) {
        c = L'_';
        break;
      }
    }
  }
  while (!leaf.empty() && (leaf.back() == L'.' || leaf.back() == L' ')) {
    leaf.pop_back();
  }
  if (leaf.empty()) return L"设备管理卡.pdf";

  // 保留名判在扩展名之前的那一段：CON.pdf 同样打不开。
  std::wstring stem = leaf.substr(0, leaf.find(L'.'));
  for (wchar_t& c : stem) {
    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
  }
  const bool reserved =
      stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
      (stem.size() == 4 &&
       ((stem.compare(0, 3, L"COM") == 0) || (stem.compare(0, 3, L"LPT") == 0)) &&
       stem[3] >= L'1' && stem[3] <= L'9');
  if (reserved) leaf = L"_" + leaf;
  // 没有扩展名就补 .pdf：文件类型筛选器给的是 PDF，缺扩展名时 PrintToPdf 写出来的
  // 文件在资源管理器里打不开。
  if (leaf.find(L'.') == std::wstring::npos) leaf += L".pdf";
  // 单个路径分量的上限是 255 个 UTF-16 单元；超了 PrintToPdf 会失败。
  if (leaf.size() > 255) {
    const std::size_t dot = leaf.rfind(L'.');
    const std::wstring ext =
        (dot == std::wstring::npos || leaf.size() - dot > 8) ? std::wstring()
                                                            : leaf.substr(dot);
    leaf = leaf.substr(0, 255 - ext.size()) + ext;
  }
  return leaf;
}

bool PickSavePath(const std::wstring& suggested_name,
                  std::filesystem::path* out, std::wstring* error) {
  ComPtr<IFileSaveDialog> dialog;
  const HRESULT created = CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                           CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dialog));
  if (FAILED(created)) {
    *error = L"保存对话框创建失败（" + HResultText(created) + L"）";
    return false;
  }
  const COMDLG_FILTERSPEC types[] = {{L"PDF 文件", L"*.pdf"}};
  dialog->SetFileTypes(1, types);
  dialog->SetDefaultExtension(L"pdf");
  dialog->SetTitle(L"导出卡片 PDF");
  // SetFileName 只收文件名：把整条路径传进去会被原样当成「文件名」显示在输入框里
  // （用户看到 D:\...\x.pdf 这种名字，空格与问号还会让保存失败）。
  // 目录部分单独走 SetFolder：路径里带目录时定位过去，文件名只留末段。
  const std::filesystem::path suggested(suggested_name);
  std::wstring leaf = SanitizeLeaf(suggested.filename().wstring());
  const std::wstring parent = suggested.parent_path().wstring();
  if (!parent.empty()) {
    std::error_code ec;
    if (std::filesystem::is_directory(parent, ec)) {
      ComPtr<IShellItem> folder;
      if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr,
                                                IID_PPV_ARGS(&folder))) &&
          folder) {
        dialog->SetFolder(folder.Get());
      }
    }
  }
  dialog->SetFileName(leaf.c_str());
  const HRESULT hr = dialog->Show(g.hwnd);
  // 取消与失败必须分开报：以前两者都落成「已取消导出」，
  // 用户看到的是「我点了保存，它说取消」，真实原因（磁盘/权限/路径）被吞掉。
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    error->clear();
    return false;
  }
  if (FAILED(hr)) {
    *error = L"保存对话框打不开（" + HResultText(hr) + L"）";
    return false;
  }

  ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(&item)) || !item) {
    *error = L"保存对话框没有返回路径";
    return false;
  }
  PWSTR raw = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) {
    *error = L"保存路径无法解析为文件系统路径";
    return false;
  }
  *out = std::filesystem::path(raw);
  CoTaskMemFree(raw);
  return true;
}

bool HasPrinter() {
  DWORD needed = 0;
  DWORD returned = 0;
  EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4,
                nullptr, 0, &needed, &returned);
  if (needed > 0) {
    std::vector<BYTE> buffer(needed);
    if (EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4,
                      buffer.data(), needed, &needed, &returned)) {
      if (returned > 0) return true;
    }
  }
  wchar_t name[512] = {};
  DWORD size = 512;
  return GetDefaultPrinterW(name, &size) != FALSE;
}

// ---------- 打印双通道 ----------

// 导出/打印前向页面要布局：字体闸门与公司名溢出都在页面里判，
// 返回 {error} 时必须中止——此时不能用系统字体顶替产出一份 PDF。
void RequestExportLayout(std::function<void(int pages, const std::string& error)> next) {
  auto handler = Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
      [next](HRESULT, LPCWSTR result) -> HRESULT {
        int pages = 0;
        std::string error;
        if (result) {
          try {
            const auto layout = nlohmann::json::parse(Narrow(result));
            if (layout.is_object()) {
              if (layout.contains("error") && layout["error"].is_string()) {
                error = layout["error"].get<std::string>();
              }
              if (layout.contains("pages") && layout["pages"].is_number_integer()) {
                pages = layout["pages"].get<int>();
              }
            }
          } catch (...) {
            error = "页面返回的导出布局无法解析";
          }
        }
        next(pages, error);
        return S_OK;
      });
  g.webview->ExecuteScript(
      L"(window.__devcardExportLayout ? window.__devcardExportLayout() : null)",
      handler.Get());
}

// 布局阶段会给 body 加 is-export，中止与完成都要摘掉。
void ExitExportMode() {
  if (g.webview) {
    g.webview->ExecuteScript(
        L"window.__devcardExportDone && window.__devcardExportDone()", nullptr);
  }
}

void ExportPdf(const std::string& suggested_name) {
  if (!g.webview || g.print_busy) return;
  g.print_busy = true;
  RequestExportLayout([suggested_name](int pages, const std::string& layout_error) {
        if (!layout_error.empty()) {
          g.print_busy = false;
          Notify(L"error", Widen(layout_error));
          return;
        }
        if (pages <= 0) {
          g.print_busy = false;
          ExitExportMode();
          Notify(L"error", L"没有可导出的卡片");
          return;
        }

        std::filesystem::path target;
        std::wstring pick_error;
        bool picked = false;
#if !defined(NDEBUG)
        // 仅调试构建：设置 DEVCARD_EXPORT_PATH 时跳过保存对话框，直接写该路径。
        // 用于验收出样张时避开模态对话框的自动化；Release 不编译这段，产品行为不变。
        wchar_t forced_path[1024] = {};
        if (GetEnvironmentVariableW(L"DEVCARD_EXPORT_PATH", forced_path,
                                    static_cast<DWORD>(std::size(forced_path))) > 0) {
          target = std::filesystem::path(forced_path);
          picked = true;
        }
#endif
        if (!picked && !PickSavePath(Widen(suggested_name), &target, &pick_error)) {
          g.print_busy = false;
          ExitExportMode();
          if (pick_error.empty()) {
            Notify(nullptr, L"已取消导出");
          } else {
            // 真实原因必须照实报：以前这里也说「已取消」，用户只看到取消却不知道为什么没存下。
            Notify(L"error", pick_error);
          }
          return;
        }

        ComPtr<ICoreWebView2_7> webview7;
        if (FAILED(g.webview.As(&webview7)) || !webview7) {
          g.print_busy = false;
          ExitExportMode();
          Notify(L"error", L"当前 WebView2 运行时不支持导出 PDF");
          return;
        }

        ComPtr<ICoreWebView2PrintSettings> settings;
        CreateA4PrintSettings(&settings);
        auto done = Callback<ICoreWebView2PrintToPdfCompletedHandler>(
            [target](HRESULT hr, BOOL ok) -> HRESULT {
              g.print_busy = false;
              ExitExportMode();
              if (SUCCEEDED(hr) && ok) {
                Notify(nullptr, L"已导出 PDF：" + target.wstring());
              } else {
                Notify(L"error", L"导出 PDF 失败（" + HResultText(hr) + L"）");
              }
              return S_OK;
            });
        // C++ SDK 里是 PrintToPdf（对应 .NET 的 PrintToPdfAsync），只导出不弹面板。
        const HRESULT hr =
            webview7->PrintToPdf(target.c_str(), settings.Get(), done.Get());
        if (FAILED(hr)) {
          g.print_busy = false;
          ExitExportMode();
          Notify(L"error", L"导出 PDF 失败（" + HResultText(hr) + L"）");
        }
      });
}

// 直接打印：走 WebView2 的系统打印对话框（Chromium 打印管线，@page 控制分页）。
void ShowPrintDialog() {
  if (!g.webview || g.print_busy) return;
  if (!HasPrinter()) {
    Notify(L"error",
           L"系统未添加打印机，请先在「设置 › 蓝牙和其他设备 › 打印机和扫描仪」"
           L"中添加，或改用导出 PDF");
    return;
  }
  ComPtr<ICoreWebView2_16> webview16;
  if (FAILED(g.webview.As(&webview16)) || !webview16) {
    Notify(L"error",
           L"当前 WebView2 Runtime 版本过低，不支持打印对话框；请更新 WebView2 "
           L"Runtime，或改用「导出 PDF」");
    return;
  }
  RequestExportLayout([webview16](int pages, const std::string& layout_error) {
        if (!layout_error.empty()) {
          ExitExportMode();
          Notify(L"error", Widen(layout_error));
          return;
        }
        if (pages <= 0) {
          ExitExportMode();
          Notify(L"error", L"没有可打印的卡片");
          return;
        }
        const HRESULT hr =
            webview16->ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND_SYSTEM);
        if (FAILED(hr)) {
          ExitExportMode();
          Notify(L"error", L"无法打开打印对话框（" + HResultText(hr) + L"）");
        }
      });
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
    } else if (action == "openDataDir") {
      OpenDataDir(Widen(message.value("path", std::string())));
    } else if (action == "openExternal") {
      OpenExternal(Widen(message.value("url", std::string())));
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
    case WM_COMMAND: {
      switch (LOWORD(wparam)) {
        case IDM_HELP_CHECKUPDATE:
          TriggerFrontendCheckUpdate();
          break;
        case IDM_HELP_ABOUT: {
          const std::wstring about =
              std::wstring(kWindowTitle) +
              L"\n物业设备台账与设备管理卡打印工具\n\n"
              L"菜单栏「帮助 → 检查更新」触发自动版本检测。";
          MessageBoxW(hwnd, about.c_str(), L"关于 DevCard",
                      MB_OK | MB_ICONINFORMATION);
          break;
        }
      }
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
                  const std::wstring url = g.origin + L"/";
                  g.webview->Navigate(url.c_str());
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

  try {
    devcard::ServerOptions options;
    options.data_dir = devcard::default_data_dir();
    options.port = 18080;
    g_server = devcard::start_server(options);
    g.data_dir = options.data_dir;
  } catch (const std::exception& error) {
    Fatal(Widen(error.what()));
    CoUninitialize();
    return 1;
  }

  g.origin = L"http://127.0.0.1:" + std::to_wstring(g_server->port());

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WndProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&window_class)) {
    Fatal(L"窗口类注册失败。");
    g_server.reset();
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
    g_server.reset();
    CoUninitialize();
    return 1;
  }

  // 菜单栏：帮助 → 检查更新 / 关于。
  // 菜单句柄归系统管理，窗口销毁时自动释放，无需手动 DestroyMenu。
  {
    HMENU hMenuBar = CreateMenu();
    HMENU hHelpMenu = CreatePopupMenu();
    AppendMenuW(hHelpMenu, MF_STRING, IDM_HELP_CHECKUPDATE, L"检查更新");
    AppendMenuW(hHelpMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hHelpMenu, MF_STRING, IDM_HELP_ABOUT, L"关于 DevCard");
    AppendMenuW(hMenuBar, MF_POPUP,
                reinterpret_cast<UINT_PTR>(hHelpMenu), L"帮助");
    SetMenu(g.hwnd, hMenuBar);
    DrawMenuBar(g.hwnd);
  }

  if (!CreateWebView()) {
    g_server.reset();
    CoUninitialize();
    return 1;
  }
  ShowWindow(g.hwnd, SW_SHOW);
  UpdateWindow(g.hwnd);

  // server 线程在 auto-update install 流程里会设 stop_requested=true + server.stop()。
  // shell 消息循环只在 WM_DESTROY 时退出，没人轮询这个 flag。
  // 这里启动一个后台线程，每 200ms 检查一次：如果 server 想退出，就 PostMessageW(WM_CLOSE)
  // 让主窗口走正常销毁流程，进程随之干净退出。
  std::thread server_exit_watcher([&]() {
    while (!g.shutdown.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      // 正常关窗时 g.hwnd 已销毁，这里不再 PostMessage；循环靠 g.shutdown 退出。
      if (g_server && g_server->stop_requested()) {
        g.server_exit_requested = true;
        PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
        break;
      }
    }
  });

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  // 先让 watcher 退出轮询，再 join；顺序反了就是之前那个「关窗后进程不退」的死锁。
  g.shutdown.store(true);
  if (server_exit_watcher.joinable()) server_exit_watcher.join();

  if (g.controller) g.controller->Close();
  g.webview.Reset();
  g.controller.Reset();
  g.env.Reset();
  g_downloads.clear();
  g_server.reset();
  CoUninitialize();
  return 0;
}
