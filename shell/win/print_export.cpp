// Windows 壳打印/导出通道的实现（P2 从 main.cpp 拆出）。
//
// 依赖方向：print_export → shell_state（状态与通知），不再回指 main.cpp。
// 页面侧协议不变：__devcardExportLayout / __devcardExportDone / printDone。
#include "print_export.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <winspool.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "shell_state.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace devcard {
namespace win {

namespace {

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

// 导出/打印前向页面要布局：字体闸门与公司名溢出都在页面里判，
// 返回 {error} 时必须中止——此时不能用系统字体顶替产出一份 PDF。
void RequestExportLayout(
    std::function<void(int pages, const std::string& error)> next) {
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

}  // namespace

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
// 稳定版 SDK（1.0.2903.40）的 ICoreWebView2_16 只有 ShowPrintUI/PrintToPdf，
// 没有 PrintDialogClosing 事件可监听关闭：释放走双通道——页面 afterprint 触发后
// 前端发 printDone（精确路径），以及 20s 兜底定时器（对话框被取消、页面收不到
// 打印事件时防 print_busy 永久卡死；定时器只放锁不清布局，与成功路径原行为一致）。
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
  // 与 ExportPdf 同一互斥：原实现只查不置 print_busy，打印面板开着期间
  // 再点导出会通过 RequestExportLayout 并发改写 is-export 布局，
  // 打出去的版式与导出的 PDF 互踩。
  g.print_busy = true;
  RequestExportLayout([webview16](int pages, const std::string& layout_error) {
        if (!layout_error.empty()) {
          g.print_busy = false;
          ExitExportMode();
          Notify(L"error", Widen(layout_error));
          return;
        }
        if (pages <= 0) {
          g.print_busy = false;
          ExitExportMode();
          Notify(L"error", L"没有可打印的卡片");
          return;
        }
        const HRESULT hr =
            webview16->ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND_SYSTEM);
        if (FAILED(hr)) {
          g.print_busy = false;
          ExitExportMode();
          Notify(L"error", L"无法打开打印对话框（" + HResultText(hr) + L"）");
          return;
        }
        KillTimer(g.hwnd, kPrintFallbackTimer);
        SetTimer(g.hwnd, kPrintFallbackTimer, 20000, nullptr);
      });
}

void FinishPrint(HWND hwnd, bool exit_export_mode) {
  KillTimer(hwnd, kPrintFallbackTimer);
  g.print_busy = false;
  if (exit_export_mode) ExitExportMode();
}

}  // namespace win
}  // namespace devcard
