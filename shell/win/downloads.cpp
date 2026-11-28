// Windows 壳下载接管的实现（P3 从 main.cpp 拆出）。
#include "downloads.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <WebView2.h>
#include <wrl.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "shell_state.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace devcard {
namespace win {

namespace {

struct DownloadWatch {
  ComPtr<ICoreWebView2DownloadOperation> operation;
  EventRegistrationToken token = {};
  bool finished = false;
};

std::vector<std::shared_ptr<DownloadWatch>> g_downloads;

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

}  // namespace

void RegisterDownloadHandling() {
  if (!g.webview) return;
  ComPtr<ICoreWebView2_4> webview4;
  if (SUCCEEDED(g.webview.As(&webview4)) && webview4) {
    EventRegistrationToken token = {};
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
}

void ClearDownloads() { g_downloads.clear(); }

}  // namespace win
}  // namespace devcard
