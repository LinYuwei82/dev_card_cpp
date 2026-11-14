#include "devcard/server.h"

#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "devcard/error.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace devcard {
namespace {

// 可执行文件所在目录：字体随包分发，不能依赖当前工作目录。
std::filesystem::path executable_dir() {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) break;
    if (length < buffer.size() - 1) {
      buffer.resize(length);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    return std::filesystem::path(buffer.data()).parent_path();
  }
#else
  std::vector<char> buffer(4096, '\0');
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length > 0) {
    buffer.resize(static_cast<std::size_t>(length));
    return std::filesystem::path(buffer.data()).parent_path();
  }
#endif
  return std::filesystem::current_path();
}

constexpr const char* kRegularFontName = "NotoSansSC-Regular.subset.ttf";

bool has_fonts(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::exists(dir / kRegularFontName, ec);
}

}  // namespace

std::filesystem::path default_font_dir() {
  if (const char* env = std::getenv("DEVCARD_FONT_DIR"); env && *env) {
    return std::filesystem::path(env);
  }
  const std::filesystem::path exe = executable_dir();
  std::vector<std::filesystem::path> candidates;
#ifdef __APPLE__
  // app bundle：DevCard.app/Contents/MacOS/DevCard → Contents/Resources/fonts
  candidates.push_back(exe / ".." / "Resources" / "fonts");
#endif
  // Windows 绿色版与从构建目录直接跑：fonts 就在 exe 旁边
  candidates.push_back(exe / "fonts");
  candidates.push_back(exe / "resources" / "fonts");
  candidates.push_back(exe / ".." / "resources" / "fonts");
  for (const auto& candidate : candidates) {
    if (has_fonts(candidate)) {
      std::error_code ec;
      const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
      return ec ? candidate : canonical;
    }
  }
  return candidates.front();
}

std::filesystem::path default_data_dir() {
#ifdef _WIN32
  if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
    return std::filesystem::path(local) / "Anye" / "DevCard";
  }
  return std::filesystem::temp_directory_path() / "Anye" / "DevCard";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / "Library" / "Application Support" /
           "Anye" / "DevCard";
  }
  return std::filesystem::temp_directory_path() / "Anye" / "DevCard";
#else
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".local" / "share" / "Anye" / "DevCard";
  }
  return std::filesystem::temp_directory_path() / "Anye" / "DevCard";
#endif
}

}  // namespace devcard
