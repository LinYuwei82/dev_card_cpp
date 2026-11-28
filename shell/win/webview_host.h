// Windows 壳的 WebView2 宿主：创建、设置、shim、导航与窗口尺寸同步。
//
// P3 从 main.cpp 拆出。事件注册按模块分工：
//   消息桥 → bridge.h、下载 → downloads.h、导航/新窗口/快捷键 → 本文件。
#pragma once

namespace devcard {
namespace win {

// 创建 WebView2 环境与控件；失败时已弹窗并销毁窗口，返回 false。
bool CreateWebView();

// WM_SIZE：让 WebView2 控件铺满客户区。
void ResizeWebView();

// server 就绪后导航到本地页面（同时打启动点）。
void NavigateToApp();

}  // namespace win
}  // namespace devcard
