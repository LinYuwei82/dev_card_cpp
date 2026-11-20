#include "devcard/paths.h"

#include "win_encoding/encoding.h"

namespace devcard {

// 路径与 UTF-8 的转换统一收敛在 modules/win_encoding：
// Windows 上 path 内部是 wchar_t，直接 .string() 会按 ANSI 代码页转换，中文路径会坏。

std::filesystem::path path_from_utf8(std::string_view utf8) {
  return win_encoding::path_from_utf8(utf8);
}

std::string path_to_utf8(const std::filesystem::path& path) {
  return win_encoding::path_to_utf8(path);
}

}  // namespace devcard
