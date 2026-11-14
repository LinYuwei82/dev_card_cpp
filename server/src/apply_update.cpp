#include "apply_update.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

#include "devcard/error.h"
#include "devcard/paths.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace devcard {
namespace {

using nlohmann::json;

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

std::string narrow(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

// 与 update_package.cpp 的规则一致：反斜杠只在引号前成对翻倍。
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

std::string last_error_text() {
  const DWORD code = GetLastError();
  return "Win32 错误 " + std::to_string(static_cast<unsigned long>(code));
}

std::string now_iso_utc() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &now);
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

// 跑一个进程并等它结束；返回 {能否启动, 退出码, 错误说明}。
struct RunResult {
  bool started = false;
  int exit_code = -1;
  std::string error;
};

RunResult run_and_wait(const std::filesystem::path& program,
                       const std::wstring& command_line,
                       const std::filesystem::path& working_dir) {
  RunResult result;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
  buffer.push_back(L'\0');
  const std::wstring cwd = working_dir.wstring();
  if (!CreateProcessW(program.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, cwd.c_str(), &startup,
                      &process)) {
    result.error = "无法启动 " + path_to_utf8(program) + "：" + last_error_text();
    return result;
  }
  result.started = true;
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  result.exit_code = static_cast<int>(code);
  return result;
}

// 结果文件用「写临时名 + MoveFileEx 替换」落盘：要么是上一份，要么是完整的新一份。
bool write_json_atomic(const std::filesystem::path& target, const json& doc) {
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  const std::filesystem::path temp = target.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << doc.dump(2);
    out.flush();
    if (!out) return false;
  }
  const std::wstring from = temp.wstring();
  const std::wstring to = target.wstring();
  if (!MoveFileExW(from.c_str(), to.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

#endif  // _WIN32

std::string now_iso_local() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
  return buffer;
}

// 跨平台原子写：先写 .tmp 再替换。Windows 的 rename 不覆盖已存在文件，
// 所以失败时删掉目标再试一次——两份状态文件都允许「后写覆盖先写」。
bool write_json_file(const std::filesystem::path& target, const json& doc,
                     std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  const std::filesystem::path temp = target.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "无法写入 " + path_to_utf8(temp);
      return false;
    }
    out << doc.dump(2);
    out.flush();
    if (!out) {
      if (error) *error = "写入失败 " + path_to_utf8(temp);
      std::filesystem::remove(temp, ec);
      return false;
    }
  }
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(target, ignored);
    std::filesystem::rename(temp, target, ec);
  }
  if (ec) {
    if (error) *error = "无法替换 " + path_to_utf8(target) + "：" + ec.message();
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

std::filesystem::path apply_state_path(const std::filesystem::path& data_dir) {
  return data_dir / "updates" / "apply-state.json";
}

}  // namespace

std::filesystem::path current_executable() {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
    if (size == 0) return {};
    if (size < buffer.size()) {
      buffer.resize(size);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  return {};
#endif
}

unsigned long current_process_id() {
#ifdef _WIN32
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return 0;
#endif
}

std::vector<std::string> current_arguments() {
#ifdef _WIN32
  int count = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::string> args;
  if (!argv) return args;
  for (int i = 1; i < count; ++i) args.push_back(narrow(argv[i]));
  LocalFree(argv);
  return args;
#else
  return {};
#endif
}

std::string read_product_version(const std::filesystem::path& exe) {
#ifdef _WIN32
  if (exe.empty()) return {};
  const std::wstring file = exe.wstring();
  DWORD unused = 0;
  const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &unused);
  if (size == 0) return {};
  std::vector<unsigned char> data(size);
  if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return {};
  void* pointer = nullptr;
  UINT length = 0;
  // Translation 固定 0x0409/1200，与 version.rc.in 一致。
  if (!VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductVersion",
                      &pointer, &length) ||
      !pointer || length == 0) {
    return {};
  }
  const wchar_t* text = static_cast<const wchar_t*>(pointer);
  return narrow(std::wstring(text, wcsnlen(text, length)));
#else
  (void)exe;
  return {};
#endif
}

void spawn_apply_update(const std::filesystem::path& helper,
                        const ApplyUpdateOptions& options) {
#ifdef _WIN32
  std::wstring command = quote_arg(helper.wstring());
  command += L" --apply-update " + quote_arg(options.setup.wstring());
  command += L" --wait-pid " + std::to_wstring(options.wait_pid);
  command += L" --result " + quote_arg(options.result.wstring());
  if (!options.relaunch.empty()) {
    command += L" --relaunch " + quote_arg(options.relaunch.wstring());
    for (const std::string& arg : options.relaunch_args) {
      command += L" --relaunch-arg " + quote_arg(widen(arg));
    }
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  const std::wstring cwd = helper.parent_path().wstring();
  // DETACHED_PROCESS：不继承本进程的控制台，服务器退出后协助器继续跑。
  if (!CreateProcessW(helper.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS, nullptr, cwd.c_str(), &startup,
                      &process)) {
    throw AppError("install_failed",
                   "无法启动更新协助程序：" + std::string(last_error_text()));
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
  (void)helper;
  (void)options;
  throw AppError("install_unsupported",
                 "自动安装目前只有 Windows 安装版支持");
#endif
}

const char* apply_state_name(ApplyState state) {
  switch (state) {
    case ApplyState::Installing:
      return "installing";
    case ApplyState::Verifying:
      return "verifying";
    case ApplyState::Done:
      return "done";
    case ApplyState::Failed:
      return "failed";
  }
  return "failed";
}

bool write_apply_state(const std::filesystem::path& data_dir, ApplyState state,
                       const std::string& detail, std::string* error) {
  const json doc = {{"state", apply_state_name(state)},
                    {"detail", detail},
                    {"updated_at", now_iso_local()}};
  return write_json_file(apply_state_path(data_dir), doc, error);
}

ApplyStateSnapshot read_apply_state(const std::filesystem::path& data_dir) {
  ApplyStateSnapshot snapshot;
  const std::filesystem::path file = apply_state_path(data_dir);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return snapshot;
  snapshot.present = true;
  const auto write_time = std::filesystem::last_write_time(file, ec);
  if (!ec) {
    snapshot.age_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::filesystem::file_time_type::clock::now() - write_time)
            .count();
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) return snapshot;
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    snapshot.state = "unknown";
    return snapshot;
  }
  snapshot.state = doc.value("state", std::string{});
  snapshot.detail = doc.value("detail", std::string{});
  snapshot.updated_at = doc.value("updated_at", std::string{});
  return snapshot;
}

bool apply_state_interrupted(const ApplyStateSnapshot& snapshot,
                             int threshold_seconds) {
  return snapshot.present && snapshot.state == "installing" &&
         snapshot.age_seconds > threshold_seconds;
}

bool write_app_started_marker(const std::filesystem::path& data_dir,
                              const std::string& version, std::string* error) {
  const json doc = {{"version", version}, {"started_at", now_iso_local()}};
  return write_json_file(data_dir / "app-started.marker", doc, error);
}

int run_apply_update(const ApplyUpdateOptions& options) {
#ifndef _WIN32
  std::cerr << "协助更新仅支持 Windows" << std::endl;
  return 1;
#else
  // 1) 等父进程退出：它退出后安装器才不会被文件占用挡住。
  if (options.wait_pid != 0) {
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE,
                                static_cast<DWORD>(options.wait_pid));
    if (parent) {
      WaitForSingleObject(parent, 120000);
      CloseHandle(parent);
    }
  }

  const std::filesystem::path installed_server =
      options.relaunch.parent_path() / L"devcard-server.exe";
  const std::string version_before = read_product_version(installed_server);

  json doc = {{"setup", path_to_utf8(options.setup)},
              {"installed_server", path_to_utf8(installed_server)},
              {"version_before", version_before},
              {"version_after", ""},
              {"exit_code", -1},
              {"error", ""},
              {"finished_at", ""}};

  std::error_code ec;
  // 协助器已接管：从这里起，中断只可能是协助器被杀或机器重启。
  std::string state_error;
  write_apply_state(options.result.parent_path(), ApplyState::Verifying,
                    "协助器已接管，正在调用安装器", &state_error);
  if (options.setup.empty() || !std::filesystem::exists(options.setup, ec)) {
    doc["error"] = "安装包不存在：" + path_to_utf8(options.setup);
  } else {
    const std::filesystem::path log = options.setup.parent_path() / L"inno-install.log";
    // 静默安装：不弹窗、不重启系统、关掉还占着程序文件的实例。
    // /LOG 让安装器自己留一份过程日志，失败时能看出卡在哪一步。
    std::wstring command = quote_arg(options.setup.wstring());
    command += L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
    command += L" /LOG=" + quote_arg(log.wstring());
    const RunResult run =
        run_and_wait(options.setup, command, options.setup.parent_path());
    if (!run.started) {
      doc["error"] = run.error;
    } else {
      doc["exit_code"] = run.exit_code;
      doc["version_after"] = read_product_version(installed_server);
    }
  }
  doc["finished_at"] = now_iso_utc();

  // 四态闭环：安装器退出码 0 且没有启动失败才算 done，其余一律 failed。
  const int exit_code = doc["exit_code"].get<int>();
  const bool succeeded = exit_code == 0 && doc["error"].get<std::string>().empty();
  write_apply_state(options.result.parent_path(),
                    succeeded ? ApplyState::Done : ApplyState::Failed,
                    succeeded ? "安装器退出码 0"
                              : "安装器退出码 " + std::to_string(exit_code),
                    &state_error);

  if (!write_json_atomic(options.result, doc)) {
    std::cerr << "警告：无法写入安装结果 " << path_to_utf8(options.result)
              << std::endl;
  }

  // 无论成败都拉起目标程序：失败时拉起的是旧版本（文件没被替换）。
  // 参数沿用原进程：--server / --port / --data-dir 必须原样回来。
  if (!options.relaunch.empty() && std::filesystem::exists(options.relaunch, ec)) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command_line = quote_arg(options.relaunch.wstring());
    for (const std::string& arg : options.relaunch_args) {
      command_line += L" " + quote_arg(widen(arg));
    }
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');
    const std::wstring cwd = options.relaunch.parent_path().wstring();
    if (CreateProcessW(options.relaunch.c_str(), buffer.data(), nullptr, nullptr,
                       FALSE, 0, nullptr, cwd.c_str(), &startup, &process)) {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
  }
  return exit_code == 0 ? 0 : 1;
#endif
}

}  // namespace devcard
