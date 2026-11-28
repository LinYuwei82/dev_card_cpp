// Windows 壳的 JS 桥：页面 postMessage 的唯一入口。
//
// P3 从 main.cpp 拆出。协议与 macOS 共用同一形状
// （window.webkit.messageHandlers.devcard），本文件只做 action 分发。
#pragma once

namespace devcard {
namespace win {

// WebView2 就绪后注册 add_WebMessageReceived。
void RegisterBridge();

}  // namespace win
}  // namespace devcard
