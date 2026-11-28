// Windows 壳的原生「关于」对话框（帮助菜单 / 状态栏版本号点击）。
//
// P3 从 main.cpp 拆出。桥的 showAbout 分支依赖本模块，所以先于 bridge 抽出，
// 避免函数仅靠外部链接暴露在 main.cpp 里。
#pragma once

#include <windows.h>

namespace devcard {
namespace win {

void ShowAboutDialog(HWND parent);

}  // namespace win
}  // namespace devcard
