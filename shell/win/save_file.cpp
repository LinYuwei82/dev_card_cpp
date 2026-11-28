// Windows 壳页面「保存文件」通道的实现。
//
// 页面（web/js/devices.js 的 saveXlsx）在桌面壳内把导出的 xlsx 内容以 base64
// 发过来；这里弹 IFileSaveDialog 让用户选路径，原子写入后把结果回给页面的
// window.__devcardSaveResult。与 macOS 的 NSSavePanel 通道同一协议：
//   ok     → { status:"ok", path }
//   cancel → { status:"cancel" }（不落文件、页面不提示）
//   error  → { status:"error", message }
#include "save_file.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wrl.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "downloads.h"
#include "shell_state.h"

using Microsoft::WRL::ComPtr;

namespace devcard {
namespace win {

namespace {

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// 标准 base64（页面用 btoa，带 '=' padding）；容忍换行与空白。
bool DecodeBase64(const std::string& text, std::vector<unsigned char>* out) {
  out->clear();
  out->reserve(text.size() * 3 / 4 + 3);
  std::uint32_t value = 0;
  int bits = 0;
  bool padded = false;
  for (const char c : text) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    if (c == '=') {
      padded = true;
      continue;
    }
    if (padded) return false;  // padding 之后不能再有数据
    const int digit = Base64Value(c);
    if (digit < 0) return false;
    value = (value << 6) | static_cast<std::uint32_t>(digit);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<unsigned char>((value >> bits) & 0xFF));
    }
  }
  return true;
}

std::wstring LowerExtension(const std::filesystem::path& path) {
  std::wstring ext = path.extension().wstring();
  for (wchar_t& c : ext) {
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  }
  return ext;
}

// 按扩展名给对话框配筛选器；默认扩展名供用户没写后缀时自动补全。
void ConfigureFilters(IFileSaveDialog* dialog, const std::wstring& ext) {
  if (ext == L".xlsx") {
    const COMDLG_FILTERSPEC types[] = {{L"Excel 工作簿 (*.xlsx)", L"*.xlsx"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"xlsx");
  } else if (ext == L".pdf") {
    const COMDLG_FILTERSPEC types[] = {{L"PDF 文件 (*.pdf)", L"*.pdf"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"pdf");
  } else {
    const COMDLG_FILTERSPEC types[] = {{L"所有文件 (*.*)", L"*.*"}};
    dialog->SetFileTypes(1, types);
    if (ext.size() > 1) dialog->SetDefaultExtension(ext.c_str() + 1);
  }
}

bool PickSavePath(const std::filesystem::path& suggested,
                  const std::wstring& title, std::filesystem::path* out,
                  std::wstring* error) {
  ComPtr<IFileSaveDialog> dialog;
  const HRESULT created =
      CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(&dialog));
  if (FAILED(created)) {
    *error = L"保存对话框创建失败（" + HResultText(created) + L"）";
    return false;
  }
  DWORD options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
  }
  dialog->SetTitle(title.c_str());
  const std::wstring ext = LowerExtension(suggested);
  ConfigureFilters(dialog.Get(), ext);
  dialog->SetFileName(suggested.filename().c_str());
  // 默认目录与浏览器下载一致：用户的「下载」文件夹。
  const std::filesystem::path dir = DownloadsDir();
  std::error_code ec;
  if (!dir.empty() && std::filesystem::is_directory(dir, ec)) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr,
                                              IID_PPV_ARGS(&folder))) &&
        folder) {
      dialog->SetFolder(folder.Get());
    }
  }

  const HRESULT shown = dialog->Show(g.hwnd);
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    error->clear();  // 取消不是错误
    return false;
  }
  if (FAILED(shown)) {
    *error = L"保存对话框打不开（" + HResultText(shown) + L"）";
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

// 先写同目录临时文件再改名替换：中途失败不会留下半个 xlsx。
bool WriteAtomically(const std::filesystem::path& target,
                     const std::vector<unsigned char>& bytes,
                     std::wstring* error) {
  std::filesystem::path temp = target;
  temp += L".devcard-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = L"无法创建目标文件（目录不可写或路径过长）";
      return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
      out.close();
      std::error_code ec;
      std::filesystem::remove(temp, ec);
      *error = L"写入文件失败（磁盘空间或权限不足）";
      return false;
    }
  }
  if (!MoveFileExW(temp.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD code = GetLastError();
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    *error = L"无法写入目标文件（" + HResultText(HRESULT_FROM_WIN32(code)) + L"）";
    return false;
  }
  return true;
}

void FinishSave(const char* status, const std::string& path,
                const std::string& message) {
  nlohmann::json payload;
  payload["status"] = status;
  if (!path.empty()) payload["path"] = path;
  if (!message.empty()) payload["message"] = message;
  const std::wstring script =
      L"window.__devcardSaveResult && window.__devcardSaveResult(" +
      Widen(payload.dump()) + L");";
  if (g.webview) {
    g.webview->ExecuteScript(script.c_str(), nullptr);
  }
}

}  // namespace

void SaveFileFromBridge(const std::string& filename, const std::string& title,
                        const std::string& content_base64) {
  std::vector<unsigned char> bytes;
  if (content_base64.empty() || !DecodeBase64(content_base64, &bytes) ||
      bytes.empty()) {
    FinishSave("error", std::string(), "导出内容为空或无法解码");
    return;
  }

  std::filesystem::path suggested =
      filename.empty() ? std::filesystem::path(L"设备台账.xlsx")
                       : std::filesystem::path(Widen(filename));
  suggested = suggested.filename();
  if (suggested.empty()) suggested = L"设备台账.xlsx";

  const std::wstring dialog_title = title.empty() ? L"保存文件" : Widen(title);
  std::filesystem::path target;
  std::wstring error;
  if (!PickSavePath(suggested, dialog_title, &target, &error)) {
    if (error.empty()) {
      FinishSave("cancel", std::string(), std::string());
    } else {
      FinishSave("error", std::string(), Narrow(error));
    }
    return;
  }
  if (!WriteAtomically(target, bytes, &error)) {
    FinishSave("error", Narrow(target.wstring()), Narrow(error));
    return;
  }
  FinishSave("ok", Narrow(target.wstring()), std::string());
}

}  // namespace win
}  // namespace devcard
