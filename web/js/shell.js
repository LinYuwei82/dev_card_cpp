import { hasNativeBridge, toast } from './core.js';

  // macOS 桌面壳：自更新交给 Sparkle，前端只负责转发触发。
  function isMacShell() {
    return /Macintosh|Mac OS X/i.test(navigator.userAgent || '') && hasNativeBridge();
  }

  // Windows WebView2 壳：有 chrome.webview + hostObjects。hostObjects 仅作
  // 「当前在 WebView2 里」的探测信号——旧更新桥已退役，win 更新走
  // server 的更新接口（state/check/download/install），页面不再注入更新 shim。
  function isWinWebview() {
    return !!(/Windows/i.test(navigator.userAgent || '') &&
      window.chrome && window.chrome.webview && window.chrome.webview.hostObjects);
  }

  // 壳层的两条输出通道共用同一份打印 DOM，只改变输出目标。
  function sendToShell(action, payload) {
    var message = payload || {};
    message.action = action;
    // WebView2 原生桥优先：不依赖注入脚本的时机（注入晚于点击时老写法会静默落空）。
    if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
      window.chrome.webview.postMessage(message);
      return true;
    }
    if (window.webkit && window.webkit.messageHandlers &&
        window.webkit.messageHandlers.devcard) {
      window.webkit.messageHandlers.devcard.postMessage(message);
      return true;
    }
    return false;
  }

  // 壳层回传导出/打印结果（见 shell/mac/main.mm）。
  window.__devcardNotify = function (kind, message) {
    toast(message, kind === 'error' ? 'error' : undefined);
  };

export { isMacShell, isWinWebview, sendToShell };
