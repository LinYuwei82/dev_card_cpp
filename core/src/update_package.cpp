#include "devcard/update_package.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "devcard/sha256.h"

namespace devcard {
namespace {

std::string ascii_lower(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

std::string read_tail(const std::filesystem::path& file, std::size_t limit) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  std::string text{std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>()};
  if (text.size() > limit) text = text.substr(text.size() - limit);
  // 压成一行：错误提示里不需要保留 curl 的进度条控制字符。
  for (char& c : text) {
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

#ifdef _WIN32

std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), size);
  return out;
}

// Windows 命令行参数引号规则（反斜杠只在引号前需要成对翻倍）。
std::wstring quote_arg(const std::wstring& value) {
  if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') {
      ++backslashes;
    } else if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      backslashes = 0;
      out.push_back(c);
    }
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

int run_process(const std::vector<std::string>& args) {
  std::wstring command;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) command.push_back(L' ');
    command += quote_arg(widen(args[i]));
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}

#else

int run_process(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    execvp(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

#endif

}  // namespace

bool is_valid_sha256(std::string_view value) {
  if (value.size() != 64) return false;
  for (char c : value) {
    if (!is_hex(c)) return false;
  }
  return true;
}

bool is_valid_update_version(std::string_view version) {
  if (version.empty() || version.size() > 40) return false;
  for (char c : version) {
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                    c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool is_trusted_update_url(std::string_view url) {
  constexpr std::string_view kScheme = "https://";
  if (url.size() <= kScheme.size()) return false;
  // scheme 必须是小写 https：不做大小写宽容，避免 "HTTPS://" 之类绕过。
  if (url.substr(0, kScheme.size()) != kScheme) return false;
  const std::string_view rest = url.substr(kScheme.size());
  const std::size_t end = rest.find_first_of("/?#");
  if (end == std::string_view::npos || rest[end] != '/') return false;
  const std::string_view authority = rest.substr(0, end);
  if (authority.empty()) return false;
  // 拒绝 userinfo 与端口：固定域名就是固定域名。
  if (authority.find('@') != std::string_view::npos) return false;
  if (authority.find(':') != std::string_view::npos) return false;
  const std::string host = ascii_lower(std::string(authority));
  static const char* const kHosts[] = {
      "github.com",
      "objects.githubusercontent.com",
      "release-assets.githubusercontent.com",
      "api.github.com",
  };
  for (const char* allowed : kHosts) {
    if (host == allowed) {
      return url.size() <= 2048;
    }
  }
  return false;
}

std::string sanitize_update_file_name(std::string_view raw) {
  std::string name(raw);
  const std::size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  for (char& c : name) {
    const unsigned char byte = static_cast<unsigned char>(c);
    const bool keep = std::isalnum(byte) || c == '.' || c == '-' || c == '_';
    if (byte < 0x20 || byte == 0x7F || !keep) c = '_';
  }
  while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
    name.pop_back();
  }
  if (name.empty() || name == "." || name == "..") return "update.bin";
  if (name.size() > 120) {
    const std::size_t dot = name.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos && name.size() - dot <= 10) {
      ext = name.substr(dot);
    }
    name = name.substr(0, 120 - ext.size()) + ext;
  }
  return name;
}

std::string validate_update_spec_code(const UpdatePackageSpec& spec) {
  if (!is_valid_update_version(spec.version)) return "manifest_invalid";
  if (!is_trusted_update_url(spec.url)) return "endpoint_not_allowed";
  if (!is_valid_sha256(spec.sha256)) return "manifest_invalid";
  if (spec.size == 0 || spec.size > kUpdateMaxBytes) return "manifest_invalid";
  return {};
}

std::string validate_update_spec(const UpdatePackageSpec& spec) {
  const std::string code = validate_update_spec_code(spec);
  if (code.empty()) return {};
  if (code == "endpoint_not_allowed") {
    return "更新包地址不在允许的固定 HTTPS 域名内";
  }
  if (!is_valid_update_version(spec.version)) return "更新清单里的版本号不合法";
  if (!is_valid_sha256(spec.sha256)) return "更新清单缺少合法的 SHA256";
  if (spec.size == 0) return "更新清单缺少字节数";
  return "更新包超过 300 MB 上限";
}

UpdateVerification verify_update_file(const std::filesystem::path& file,
                                      std::uint64_t expected_size,
                                      std::string_view expected_sha256) {
  UpdateVerification result;
  if (!is_valid_sha256(expected_sha256)) {
    result.code = "manifest_invalid";
    result.error = "清单里的 SHA256 不合法";
    return result;
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) {
    result.code = "download_failed";
    result.error = "无法读取更新包大小：" + ec.message();
    return result;
  }
  result.actual_size = static_cast<std::uint64_t>(size);
  if (result.actual_size != expected_size) {
    result.code = "download_failed";
    result.error = "字节数不符：清单 " + std::to_string(expected_size) +
                   "，实际 " + std::to_string(result.actual_size);
    return result;
  }
  try {
    result.actual_sha256 = sha256_file(file);
  } catch (const std::exception& error) {
    result.code = "download_failed";
    result.error = std::string("无法计算 SHA256：") + error.what();
    return result;
  }
  if (ascii_lower(result.actual_sha256) != ascii_lower(std::string(expected_sha256))) {
    result.code = "checksum_mismatch";
    result.error = "SHA256 不符：清单 " + std::string(expected_sha256) +
                   "，实际 " + result.actual_sha256;
    return result;
  }
  result.ok = true;
  return result;
}

bool curl_fetch(const std::string& url, const std::filesystem::path& target,
                std::string* error) {
  const char* override_path = std::getenv("DEVCARD_CURL");
  const std::string curl =
      (override_path && *override_path) ? std::string(override_path)
                                        : std::string("curl");
  std::filesystem::path log = target;
  log += ".log";
  std::error_code ec;
  std::filesystem::remove(log, ec);

  const std::vector<std::string> args = {
      curl,
      "--fail",
      "--silent",
      "--show-error",
      "--location",
      // 初始地址与重定向都只允许 https；不会降级到 http。
      "--proto",
      "=https",
      "--proto-redir",
      "=https",
      "--tlsv1.2",
      "--max-time",
      "900",
      "--max-filesize",
      std::to_string(kUpdateMaxBytes),
      "--output",
      target.string(),
      "--stderr",
      log.string(),
      url,
  };
  const int code = run_process(args);
  if (code == 0) {
    std::filesystem::remove(log, ec);
    return true;
  }
  std::string detail;
  if (code < 0) {
    detail = "无法启动 curl（系统未提供或不可执行）";
  } else {
    detail = "curl 退出码 " + std::to_string(code);
  }
  const std::string tail = read_tail(log, 400);
  if (!tail.empty()) detail += "：" + tail;
  std::filesystem::remove(log, ec);
  if (error) *error = detail;
  return false;
}

UpdateDownloadOutcome download_update_package(
    const std::filesystem::path& updates_dir, const UpdatePackageSpec& spec,
    const UpdateFetcher& fetcher) {
  UpdateDownloadOutcome outcome;
  const std::string invalid = validate_update_spec(spec);
  if (!invalid.empty()) {
    outcome.error_code = validate_update_spec_code(spec);
    outcome.error = invalid;
    return outcome;
  }

  std::error_code ec;
  std::filesystem::create_directories(updates_dir, ec);
  if (ec) {
    outcome.error_code = "download_failed";
    outcome.error = "无法创建 updates 目录：" + ec.message();
    return outcome;
  }

  const std::filesystem::path final_path =
      updates_dir / sanitize_update_file_name(spec.file_name);
  // 已下载且校验通过的直接复用，避免重复占带宽。
  if (std::filesystem::exists(final_path, ec) && !ec) {
    const UpdateVerification existing =
        verify_update_file(final_path, spec.size, spec.sha256);
    if (existing.ok) {
      outcome.ok = true;
      outcome.path = final_path.string();
      outcome.bytes = existing.actual_size;
      outcome.sha256 = existing.actual_sha256;
      return outcome;
    }
    std::filesystem::remove(final_path, ec);
  }

  std::filesystem::path part = final_path;
  part += ".part";
  std::filesystem::remove(part, ec);

  std::string fetch_error;
  if (!fetcher || !fetcher(spec.url, part, &fetch_error)) {
    std::filesystem::remove(part, ec);
    outcome.error_code = "download_failed";
    outcome.error = "下载失败：" + (fetch_error.empty() ? "未知原因" : fetch_error);
    return outcome;
  }

  const UpdateVerification checked =
      verify_update_file(part, spec.size, spec.sha256);
  if (!checked.ok) {
    std::filesystem::remove(part, ec);
    outcome.error_code =
        checked.code.empty() ? "download_failed" : checked.code;
    outcome.error = "更新包校验失败，已删除残件：" + checked.error;
    return outcome;
  }

  std::filesystem::remove(final_path, ec);
  std::filesystem::rename(part, final_path, ec);
  if (ec) {
    std::filesystem::remove(part, ec);
    outcome.error_code = "download_failed";
    outcome.error = "更新包落盘失败：" + ec.message();
    return outcome;
  }
  outcome.ok = true;
  outcome.path = final_path.string();
  outcome.bytes = checked.actual_size;
  outcome.sha256 = checked.actual_sha256;
  return outcome;
}

void cleanup_update_partials(const std::filesystem::path& updates_dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(updates_dir, ec)) return;
  for (const auto& entry : std::filesystem::directory_iterator(updates_dir, ec)) {
    if (ec) break;
    const std::string name = entry.path().filename().string();
    const bool partial =
        name.size() >= 5 && name.compare(name.size() - 5, 5, ".part") == 0;
    const bool log = name.size() >= 5 &&
                     name.compare(name.size() - 5, 5, ".log") == 0;
    if (partial || log) std::filesystem::remove(entry.path(), ec);
  }
}

}  // namespace devcard
