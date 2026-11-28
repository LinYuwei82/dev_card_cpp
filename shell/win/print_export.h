// Windows 壳的打印/导出双通道。
//
// P2 从 main.cpp 拆出。两条通道共用同一把 print_busy 互斥：
//   导出 — 页面布局校验 → 保存对话框 → WebView2 PrintToPdf
//   打印 — 页面布局校验 → WebView2 系统打印对话框
// 布局阶段页面会给 body 加 is-export，失败与结束都要摘掉，统一走 FinishPrint。
#pragma once

#include <windows.h>

#include <string>

namespace devcard {
namespace win {

// 打印对话框 20s 兜底定时器（WM_TIMER）：对话框被取消且页面没发 printDone 时，
// 防止 print_busy 永久卡死。
constexpr UINT_PTR kPrintFallbackTimer = 0x4C05;

void ExportPdf(const std::string& suggested_name);
void ShowPrintDialog();

// 打印流程收尾：释放互斥并撤销兜底定时器。
// exit_export_mode=true 同时摘掉页面的 is-export 布局（printDone 与各失败路径）；
// false 只放锁不动布局（兜底路径无法区分「已取消」与「仍开着」，清布局可能
// 破坏尚未出纸的打印，精确清理走 printDone）。
void FinishPrint(HWND hwnd, bool exit_export_mode);

}  // namespace win
}  // namespace devcard
