// Windows 壳「关于」对话框的实现（P3 从 main.cpp 拆出）。
//
// 版本/构建号来自编译期注入（VERSION.txt + tag 序号），与更新链路清单版本
// 同源；链接用 SYSLINK 控件，打开 OSS 更新记录/手册与 GitHub 发布页。
#include "about_dialog.h"

#include <commctrl.h>

#include "devcard/update_manifest.h"

#include "shell_state.h"

namespace devcard {
namespace win {

namespace {

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

}  // namespace

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

}  // namespace win
}  // namespace devcard
