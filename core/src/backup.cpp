#include "devcard/backup.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <string>

#include "devcard/error.h"
#include "devcard/dao/backup_dao.h"
#include "devcard/db.h"
#include "devcard/paths.h"

namespace devcard {
namespace {

std::string stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
  return buffer;
}

std::string format_time(std::filesystem::file_time_type time) {
  using namespace std::chrono;
  const auto system_time =
      time_point_cast<system_clock::duration>(
          time - std::filesystem::file_time_type::clock::now() +
          system_clock::now());
  const std::time_t t = system_clock::to_time_t(system_time);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
  return buffer;
}

BackupEntry describe(const std::filesystem::path& file) {
  BackupEntry entry;
  entry.name = path_to_utf8(file.filename());
  entry.path = path_to_utf8(file);
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  entry.bytes = ec ? 0 : static_cast<std::int64_t>(size);
  const auto mtime = std::filesystem::last_write_time(file, ec);
  if (!ec) entry.created_at = format_time(mtime);
  return entry;
}

}  // namespace

std::filesystem::path backups_dir(const std::filesystem::path& data_dir) {
  return data_dir / "backups";
}

BackupEntry create_backup(Db& db, const std::filesystem::path& data_dir,
                          std::string_view label) {
  const std::filesystem::path dir = backups_dir(data_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) throw AppError("backup_failed", "无法创建备份目录: " + ec.message());

  std::string safe_label(label);
  for (char& c : safe_label) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') c = '-';
  }
  if (safe_label.empty()) safe_label = "auto";

  std::filesystem::path target =
      dir / ("devcard-" + stamp() + "-" + safe_label + ".db");
  int suffix = 2;
  while (std::filesystem::exists(target, ec)) {
    target = dir / ("devcard-" + stamp() + "-" + safe_label + "-" +
                    std::to_string(suffix++) + ".db");
  }

  db.backup().vacuum_into(db, path_to_utf8(target));
  return describe(target);
}

std::vector<BackupEntry> list_backups(const std::filesystem::path& data_dir) {
  std::vector<BackupEntry> entries;
  std::error_code ec;
  const std::filesystem::path dir = backups_dir(data_dir);
  if (!std::filesystem::exists(dir, ec)) return entries;
  for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!item.is_regular_file()) continue;
    if (item.path().extension() != ".db") continue;
    entries.push_back(describe(item.path()));
  }
  std::sort(entries.begin(), entries.end(),
            [](const BackupEntry& a, const BackupEntry& b) {
              return a.created_at > b.created_at;
            });
  return entries;
}

void restore_backup(Db& db, const std::filesystem::path& data_dir,
                    std::string_view name) {
  if (name.empty() || name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos || name == "." || name == "..") {
    throw AppError("bad_backup", "非法的备份文件名");
  }
  // name 来自 JSON（UTF-8），不能当窄字符直接拼路径。
  const std::filesystem::path file = backups_dir(data_dir) / path_from_utf8(name);
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    throw AppError("not_found", "备份不存在");
  }

  // 备份必须是可用的本程序数据库，避免把任意文件恢复进来。
  db.backup().verify_backup_file(file);

  create_backup(db, data_dir, "pre-restore");

  db.backup().restore_from(db, path_to_utf8(file));
}

}  // namespace devcard
