#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace devcard {

std::string sha256_hex(std::string_view input);

// 读取文件全部字节后计算 SHA256；文件不存在或不可读时抛 AppError。
std::string sha256_file(const std::filesystem::path& file);

}  // namespace devcard
