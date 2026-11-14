#include "devcard/paths.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace devcard {

std::filesystem::path path_from_utf8(std::string_view utf8) {
#ifdef _WIN32
  if (utf8.empty()) return {};
  const int size = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), size);
  return std::filesystem::path(wide);
#else
  // POSIX 的窄编码就是 UTF-8，原样使用。
  return std::filesystem::path(std::string(utf8));
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::wstring wide = path.wstring();
  if (wide.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr,
                                         0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), size, nullptr, nullptr);
  return out;
#else
  return path.string();
#endif
}

}  // namespace devcard
