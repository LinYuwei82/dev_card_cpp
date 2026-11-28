// Windows 壳页面「保存文件」通道（导出设备台账 / 下载导入模板）。
//
// 页面把 xlsx 内容以 base64 交过来，壳只负责「弹系统保存对话框 + 原子写入」，
// 再把结果回给 window.__devcardSaveResult；与 macOS 的 NSSavePanel 同一协议。
#pragma once

#include <string>

namespace devcard {
namespace win {

// filename / title 为页面给出的建议文件名与对话框标题（均 UTF-8）。
// content_base64 是页面读好的完整文件内容。
void SaveFileFromBridge(const std::string& filename, const std::string& title,
                        const std::string& content_base64);

}  // namespace win
}  // namespace devcard
