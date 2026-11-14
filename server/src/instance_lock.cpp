#include "instance_lock.h"

#include <string>

#include "devcard/db.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace devcard {
namespace {

constexpr const char* kLockFileName = "devcard.lock";

std::string path_text(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
  return path.string();
#endif
}

// GetFinalPathNameByHandle 返回 \\?\D:\... 或 \\?\UNC\server\share；
// 报错文案里还原成用户能认的写法。
std::string display_path(const std::filesystem::path& path) {
  std::string text = path_text(path);
  const std::string prefix = "\\\\?\\UNC\\";
  if (text.rfind(prefix, 0) == 0) return "\\\\" + text.substr(prefix.size());
  const std::string long_prefix = "\\\\?\\";
  if (text.rfind(long_prefix, 0) == 0) return text.substr(long_prefix.size());
  return text;
}

// 规范化目录：展开 junction/symlink/8.3 短名，锁文件由此落到同一个文件对象上。
std::filesystem::path canonical_directory(const std::filesystem::path& path,
                                          std::error_code& ec) {
#ifdef _WIN32
  HANDLE handle = CreateFileW(path.c_str(), 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
      const DWORD length = GetFinalPathNameByHandleW(
          handle, buffer.data(), static_cast<DWORD>(buffer.size()),
          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (length == 0) break;
      if (length < buffer.size()) {
        buffer.resize(length);
        CloseHandle(handle);
        return std::filesystem::path(buffer);
      }
      buffer.resize(length + 1);
    }
    CloseHandle(handle);
  }
#endif
  return std::filesystem::weakly_canonical(path, ec);
}

std::string instance_message(const std::filesystem::path& dir,
                             const std::string& owner) {
  std::string message =
      "数据目录已被另一个 DevCard 进程占用：" + display_path(dir) + "\n";
  message += "占用者: " + (owner.empty() ? std::string("未知（锁文件无内容）") : owner) +
             "\n";
  message +=
      "如果确认没有其他实例，请先结束该进程；不要删除 devicedb.db 或 -wal/-shm。";
  return message;
}

}  // namespace

struct InstanceGuard::Impl {
  std::filesystem::path lock_path;
  std::string owner;
#ifdef _WIN32
  HANDLE lock_file = INVALID_HANDLE_VALUE;
  OVERLAPPED overlapped{};
  HANDLE db_mutex = nullptr;
#else
  int lock_fd = -1;
#endif
};

InstanceGuard::InstanceGuard() : impl_(std::make_unique<Impl>()) {}

InstanceGuard::~InstanceGuard() {
#ifdef _WIN32
  if (impl_->lock_file != INVALID_HANDLE_VALUE) {
    UnlockFileEx(impl_->lock_file, 0, 1, 0, &impl_->overlapped);
    CloseHandle(impl_->lock_file);
  }
  if (impl_->db_mutex) CloseHandle(impl_->db_mutex);
#else
  if (impl_->lock_fd >= 0) {
    flock(impl_->lock_fd, LOCK_UN);
    close(impl_->lock_fd);
  }
#endif
}

void InstanceGuard::set_owner_info(const std::string& info) {
  impl_->owner = info;
#ifdef _WIN32
  HANDLE& handle = impl_->lock_file;
  if (handle == INVALID_HANDLE_VALUE) return;
  // 第 0 字节留给 LockFileEx；占用者信息从第 1 字节开始写。
  // Windows 的字节范围锁是强制的：锁住第 0 字节后，别的进程从 0 读会失败，
  // 从 1 读仍然可以，所以第二个实例能读到「占用者是谁」。
  LARGE_INTEGER offset{};
  offset.QuadPart = 1;
  SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN);
  SetEndOfFile(handle);
  DWORD written = 0;
  WriteFile(handle, impl_->owner.data(), static_cast<DWORD>(impl_->owner.size()),
            &written, nullptr);
  FlushFileBuffers(handle);
#else
  if (impl_->lock_fd < 0) return;
  ftruncate(impl_->lock_fd, 0);
  lseek(impl_->lock_fd, 0, SEEK_SET);
  const ssize_t ignored =
      write(impl_->lock_fd, impl_->owner.data(), impl_->owner.size());
  (void)ignored;
#endif
}

std::unique_ptr<InstanceGuard> InstanceGuard::acquire(
    const std::filesystem::path& data_dir, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec) {
    if (error) {
      *error = "无法创建数据目录: " + path_text(data_dir) + "（" + ec.message() + "）";
    }
    return nullptr;
  }

  const std::filesystem::path dir = canonical_directory(data_dir, ec);
  const std::filesystem::path lock_path = dir / kLockFileName;

  auto guard = std::unique_ptr<InstanceGuard>(new InstanceGuard());
  guard->impl_->lock_path = lock_path;
#ifdef _WIN32
  const unsigned long pid = GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
  guard->impl_->owner =
      "PID " + std::to_string(pid) + "，启动于 " + now_text();

#ifdef _WIN32
  HANDLE handle = CreateFileW(lock_path.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "无法打开实例锁文件 " + path_text(lock_path) + "（错误码 " +
               std::to_string(GetLastError()) + "）";
    }
    return nullptr;
  }
  guard->impl_->lock_file = handle;
  guard->impl_->overlapped = OVERLAPPED{};
  if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                  1, 0, &guard->impl_->overlapped)) {
    std::string owner;
    LARGE_INTEGER offset{};
    offset.QuadPart = 1;
    SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN);
    char buffer[512] = {};
    DWORD read = 0;
    if (ReadFile(handle, buffer, sizeof(buffer) - 1, &read, nullptr)) {
      owner.assign(buffer, read);
    }
    CloseHandle(handle);
    guard->impl_->lock_file = INVALID_HANDLE_VALUE;
    if (error) *error = instance_message(dir, owner);
    return nullptr;
  }
  // 抢到锁以后把占用者信息写进去：第二个实例失败时能读到是谁。
  guard->set_owner_info(guard->impl_->owner);
  return guard;
#else
  const int fd = open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    if (error) {
      *error = "无法打开实例锁文件 " + path_text(lock_path);
    }
    return nullptr;
  }
  guard->impl_->lock_fd = fd;
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    std::string owner;
    char buffer[512] = {};
    const ssize_t read = pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (read > 0) owner.assign(buffer, static_cast<std::size_t>(read));
    close(fd);
    guard->impl_->lock_fd = -1;
    if (error) *error = instance_message(dir, owner);
    return nullptr;
  }
  guard->set_owner_info(guard->impl_->owner);
  return guard;
#endif
}

bool InstanceGuard::claim_database(const std::filesystem::path& db_file,
                                   std::string* error) {
#ifdef _WIN32
  HANDLE file = CreateFileW(db_file.c_str(), 0,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) return true;  // 交给 SQLite 报具体错误
  BY_HANDLE_FILE_INFORMATION info{};
  const bool ok = GetFileInformationByHandle(file, &info) != 0;
  CloseHandle(file);
  if (!ok) return true;

  const unsigned long long index =
      (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) |
      info.nFileIndexLow;
  const std::wstring name = L"Local\\DevCard-Db-" +
                            std::to_wstring(info.dwVolumeSerialNumber) + L"-" +
                            std::to_wstring(index);
  HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
  if (!mutex) return true;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    if (error) {
      *error = "数据库文件已被另一个 DevCard 进程以另一个路径打开：" +
               path_text(db_file) +
               "\n请确认只有一个 server 在写这个库；不要同时用两个 --data-dir "
               "指向同一份 devicedb.db。";
    }
    return false;
  }
  impl_->db_mutex = mutex;
  return true;
#else
  // POSIX：规范化目录锁已覆盖符号链接/别名；hardlink 场景未覆盖（见方案 §3.7 第 3 层）。
  (void)db_file;
  (void)error;
  return true;
#endif
}

}  // namespace devcard
