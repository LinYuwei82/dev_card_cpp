#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace devcard {

// UTF-8 字节串 ↔ std::filesystem::path。
//
// Windows 上 filesystem::path 的窄字符构造函数按 ACP（简中系统即 GBK）解释字节，
// 源码里的 UTF-8 字面量直接传进去会落盘成乱码（例：`初始账号密码.txt` 变成
// 13 个无意义的 UTF-16 码元）。凡是「常量 / JSON / 命令行走进来的 UTF-8 文本」
// 要变成路径，一律走 path_from_utf8；反过来，路径要进 JSON、日志、错误消息，
// 一律走 path_to_utf8。
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& path);

}  // namespace devcard
