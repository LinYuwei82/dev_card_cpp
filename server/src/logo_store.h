#pragma once

// 公司图标（打印卡片头部）与内嵌 logo/favicon 的读取助手。
// 自定义图标落盘的相对路径由服务端自己写，读回时仍做穿越校验。

#include <filesystem>
#include <string>
#include <string_view>

namespace devcard::detail {

inline constexpr std::string_view kLogoSettingKey = "company_logo_path";

// 只接受服务端自己写的相对路径，防路径穿越。
bool safe_logo_relative(const std::string& path);

std::string read_file_bytes(const std::filesystem::path& path);

// 内嵌静态资源以十六进制串存放，统一在这里解码。
std::string hex_to_bytes(std::string_view hex);

// 内嵌资源的十六进制串解码一次后缓存。
const std::string& logo_jpeg();
const std::string& favicon_ico();

}  // namespace devcard::detail
