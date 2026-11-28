// Windows 壳 JS 桥的实现（P3 从 main.cpp 拆出）。
//
// 壳内不得复制页面侧判定逻辑：这里只把 action 转给对应模块。
#include "bridge.h"

#include <windows.h>

#include <WebView2.h>
#include <wrl.h>

#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "about_dialog.h"
#include "print_export.h"
#include "save_file.h"
#include "shell_state.h"
#include "updater_win.h"

using Microsoft::WRL::Callback;

namespace devcard {
namespace win {

namespace {

void HandleBridgeMessage(const std::wstring& json_text) {
  try {
    const nlohmann::json message = nlohmann::json::parse(Narrow(json_text));
    if (!message.is_object()) return;
    const std::string action = message.value("action", std::string());
    if (action == "exportPdf") {
      ExportPdf(message.value("filename", std::string("设备管理卡.pdf")));
    } else if (action == "saveFile") {
      // 导出设备台账 / 下载导入模板：弹系统保存对话框后原子写入。
      SaveFileFromBridge(message.value("filename", std::string()),
                         message.value("title", std::string()),
                         message.value("contentBase64", std::string()));
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

}  // namespace

void RegisterBridge() {
  if (!g.webview) return;
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
}

}  // namespace win
}  // namespace devcard
