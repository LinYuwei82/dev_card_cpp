// Windows 壳的下载接管。
//
// P3 从 main.cpp 拆出。壳接管 WebView2 的下载面板：目标目录固定为下载文件夹，
// 同名文件追加序号，完成后通过页面通知提示落盘位置。
#pragma once

namespace devcard {
namespace win {

// WebView2 就绪后注册 add_DownloadStarting。
void RegisterDownloadHandling();

// 进程退出前清空回调持有的下载操作（原 main.cpp 的 g_downloads.clear()）。
void ClearDownloads();

}  // namespace win
}  // namespace devcard
