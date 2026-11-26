#include "devcard/server.h"

#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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

constexpr const char* kRegularFontName = "NotoSansSC-Regular.subset.ttf";

bool has_fonts(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::exists(dir / kRegularFontName, ec);
}

#ifdef _WIN32
constexpr const char* kInstallerUninstaller = "unins000.exe";

std::wstring lower_wide(std::wstring text) {
  for (wchar_t& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
  return text;
}

bool path_within(const std::filesystem::path& child,
                 const std::filesystem::path& parent) {
  std::error_code ec;
  auto a = std::filesystem::weakly_canonical(child, ec);
  if (ec) a = child;
  auto b = std::filesystem::weakly_canonical(parent, ec);
  if (ec) b = parent;
  const std::wstring left = lower_wide(a.native());
  const std::wstring right = lower_wide(b.native());
  if (left.size() <= right.size()) return false;
  if (left.compare(0, right.size(), right) != 0) return false;
  return left[right.size()] == L'\\' || left[right.size()] == L'/';
}

// 安装版判定：Inno 会在程序目录放 unins000.exe；即使用户改了安装路径，
// 也能凭它认出来。再用默认安装目录做兜底（防止卸载器被手工删掉）。
bool installed_layout(const std::filesystem::path& exe) {
  std::error_code ec;
  if (std::filesystem::exists(exe / kInstallerUninstaller, ec)) return true;
  const char* local = std::getenv("LOCALAPPDATA");
  if (!local || !*local) return false;
  return path_within(exe,
                     std::filesystem::path(local) / "Programs" / "DevCard");
}

std::string unicode_to_utf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

[[noreturn]] void fail_with_candidates(const std::string& reason,
                                       const std::filesystem::path& dir,
                                       bool installed,
                                       const std::filesystem::path& exe) {
  // 这里只在「形态默认」被采用时触发，所以 CLI / 环境变量 / 配置文件
  // 确实都没有提供，列出来是为了让用户知道下一步该从哪覆盖。
  throw AppError(
      "data_dir_unwritable",
      "数据目录不可用，已停止启动（不会自动改用其他目录，避免出现空台账）：\n"
      "  目标目录: " + unicode_to_utf8(dir.wstring()) + "\n"
      "  原因: " + reason + "\n"
      "  形态: " + std::string(installed ? "安装版" : "便携版") +
          "（程序目录 " + unicode_to_utf8(exe.wstring()) + "）\n"
      "请修正该目录权限，或显式指定以下任一来源：\n"
      "  1) 命令行 --data-dir <目录>\n"
      "  2) 环境变量 DEVCARD_DATA_DIR=<目录>\n"
      "  3) 配置文件 devcard-server.json 的 data_dir\n"
      "  形态默认: " +
          std::string(installed ? "%LOCALAPPDATA%\\DevCard\\data"
                                : "<exe 同级>\\data"));
}

// 建目录 + 写一个探测文件；任何一种失败都直接报错，绝不换目录。
void ensure_writable(const std::filesystem::path& dir, bool installed,
                     const std::filesystem::path& exe) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    fail_with_candidates("无法创建目录：" + ec.message(), dir, installed, exe);
  }
  const std::filesystem::path probe =
      dir / (".devcard-write-probe-" + std::to_string(GetCurrentProcessId()));
  {
    std::ofstream out(probe, std::ios::binary | std::ios::trunc);
    if (!out) fail_with_candidates("无法创建探测文件", dir, installed, exe);
    out << "devcard";
    out.close();
    if (out.fail()) {
      fail_with_candidates("探测文件写入失败", dir, installed, exe);
    }
  }
  std::filesystem::remove(probe, ec);
}
#endif

}  // namespace

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
#elif defined(__linux__)
  std::vector<char> buffer(4096, '\0');
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length > 0) {
    buffer.resize(static_cast<std::size_t>(length));
    return std::filesystem::path(buffer.data()).parent_path();
  }
#endif
  return std::filesystem::current_path();
}

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
  const std::filesystem::path exe = executable_dir();
  const bool installed = installed_layout(exe);
  const char* local = std::getenv("LOCALAPPDATA");
  if (installed && (!local || !*local)) {
    fail_with_candidates("环境变量 LOCALAPPDATA 未设置",
                         std::filesystem::path("%LOCALAPPDATA%") / "DevCard" /
                             "data",
                         true, exe);
  }
  // 安装版：每用户目录（不需要管理员、不配 ACL，与 macOS 同构）；
  // 便携版：仍留 exe 同级 data\，绿色版不往用户目录写东西。
  const std::filesystem::path dir =
      installed ? std::filesystem::path(local) / "DevCard" / "data"
                : exe / "data";
  ensure_writable(dir, installed, exe);
  if (installed) {
    // 早期开发版的默认目录（%LOCALAPPDATA%\Anye\DevCard）。
    // 只提示、绝不自动切换或合并——静默换目录等于给用户一个空台账。
    std::error_code ec;
    const std::filesystem::path early =
        std::filesystem::path(local) / "Anye" / "DevCard";
    if (std::filesystem::exists(early / "devicedb.db", ec) &&
        !std::filesystem::exists(dir / "devicedb.db", ec)) {
      std::cerr << "提示：检测到早期默认数据目录 "
                << unicode_to_utf8(early.wstring())
                << " 内有 devicedb.db；本次不会自动使用。如需沿用，请加 "
                   "--data-dir 指向该目录。"
                << std::endl;
    }
  }
  return dir;
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

bool is_installed_layout() {
#ifdef _WIN32
  return installed_layout(executable_dir());
#else
  return false;
#endif
}

}  // namespace devcard
