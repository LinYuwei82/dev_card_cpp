#include "devcard/backup.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>

#include "devcard/error.h"
#include "devcard/paths.h"

namespace devcard {
namespace {

struct SqliteCloser {
  void operator()(sqlite3* conn) const {
    if (conn) sqlite3_close(conn);
  }
};
using SqlitePtr = std::unique_ptr<sqlite3, SqliteCloser>;

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

SqlitePtr open_readonly(const std::filesystem::path& file) {
  sqlite3* raw = nullptr;
  const int rc = sqlite3_open_v2(path_to_utf8(file).c_str(), &raw,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
  if (rc != SQLITE_OK) {
    std::string message = raw ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
    if (raw) sqlite3_close(raw);
    throw AppError("backup_invalid", "无法打开备份文件: " + message);
  }
  return SqlitePtr(raw);
}

bool table_exists(sqlite3* conn, const char* name) {
  Stmt stmt(conn, "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?");
  stmt.bind(1, name);
  return stmt.step();
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

  db.write_raw([&](sqlite3* conn) {
    Stmt stmt(conn, "VACUUM INTO ?");
    stmt.bind(1, path_to_utf8(target));
    stmt.run();
  });
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
  {
    SqlitePtr conn = open_readonly(file);
    for (const char* table : {"device", "app_user", "app_setting",
                              "schema_migration"}) {
      if (!table_exists(conn.get(), table)) {
        throw AppError("bad_backup", "备份缺少表 " + std::string(table));
      }
    }
    Stmt stmt(conn.get(), "PRAGMA integrity_check");
    const std::string result = stmt.step() ? stmt.text(0) : std::string();
    if (result != "ok") {
      throw AppError("bad_backup", "备份完整性检查失败: " + result);
    }
  }

  create_backup(db, data_dir, "pre-restore");

  // ATTACH/DETACH 不能在事务内执行：先挂载，再开写事务，提交后卸载。
  db.write_raw([&](sqlite3* conn) {
    {
      Stmt attach(conn, "ATTACH DATABASE ? AS restore_src");
      attach.bind(1, path_to_utf8(file));
      attach.run();
    }
    try {
      exec_sql(conn, "BEGIN IMMEDIATE");
      exec_sql(conn, "DELETE FROM device");
      exec_sql(conn,
               "INSERT INTO device(id,dev_name,location,control_range,phone,"
               "created_at,updated_at) SELECT id,dev_name,location,"
               "control_range,phone,created_at,updated_at FROM restore_src.device");
      exec_sql(conn, "DELETE FROM app_user");
      exec_sql(conn,
               "INSERT INTO app_user(user_name,password_hash,display_name,role,"
               "must_change_password,failed_count,locked_until,created_at,"
               "updated_at) SELECT user_name,password_hash,display_name,role,"
               "must_change_password,failed_count,locked_until,created_at,"
               "updated_at FROM restore_src.app_user");
      exec_sql(conn, "DELETE FROM app_setting");
      exec_sql(conn, "INSERT INTO app_setting(key,value) SELECT key,value FROM restore_src.app_setting");
      exec_sql(conn, "COMMIT");
    } catch (...) {
      sqlite3_exec(conn, "ROLLBACK", nullptr, nullptr, nullptr);
      sqlite3_exec(conn, "DETACH DATABASE restore_src", nullptr, nullptr, nullptr);
      throw;
    }
    exec_sql(conn, "DETACH DATABASE restore_src");
  });
}

}  // namespace devcard
