#include "devcard/legacy.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/migrations.h"
#include "devcard/models.h"
#include "devcard/password.h"
#include "devcard/settings.h"
#include "sha256.h"

namespace devcard {
namespace {

struct LegacyDevice {
  std::int64_t id = 0;
  std::string name;
  std::string location;
  std::string control_range;
  std::string phone;
};

struct LegacyUser {
  std::string name;
  std::string password;
};

struct SqliteCloser {
  void operator()(sqlite3* conn) const {
    if (conn) sqlite3_close(conn);
  }
};
using SqlitePtr = std::unique_ptr<sqlite3, SqliteCloser>;

std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::size_t utf8_length(const std::string& value) {
  std::size_t count = 0;
  for (unsigned char c : value) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

std::string truncate_utf8(const std::string& value, std::size_t max_chars) {
  std::size_t chars = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if ((static_cast<unsigned char>(value[i]) & 0xC0) == 0x80) continue;
    if (chars == max_chars) return value.substr(0, i);
    ++chars;
  }
  return value;
}

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

std::string path_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
  return path.string();
#endif
}

SqlitePtr open_readonly(const std::filesystem::path& file) {
  sqlite3* raw = nullptr;
  const int rc = sqlite3_open_v2(path_utf8(file).c_str(), &raw,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
  if (rc != SQLITE_OK) {
    std::string message = raw ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
    if (raw) sqlite3_close(raw);
    throw AppError("legacy_read",
                   "无法只读打开旧库 " + path_utf8(file) + ": " + message);
  }
  sqlite3_busy_timeout(raw, 5000);
  return SqlitePtr(raw);
}

bool table_exists(sqlite3* conn, const char* name) {
  Stmt stmt(conn, "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?");
  stmt.bind(1, name);
  return stmt.step();
}

std::int64_t count_rows(sqlite3* conn, const char* table) {
  if (!table_exists(conn, table)) return 0;
  std::int64_t total = 0;
  Stmt stmt(conn, std::string("SELECT COUNT(*) FROM ") + table);
  if (stmt.step()) total = stmt.i64(0);
  return total;
}

std::set<std::string> table_columns(sqlite3* conn, const char* table) {
  std::set<std::string> columns;
  Stmt stmt(conn, std::string("PRAGMA table_info(") + table + ")");
  while (stmt.step()) columns.insert(stmt.text(1));
  return columns;
}

std::string column_or_null(const std::set<std::string>& columns,
                           const char* name) {
  return columns.count(name) ? std::string(name) : std::string("NULL");
}

std::vector<LegacyDevice> read_devices(sqlite3* conn) {
  std::vector<LegacyDevice> devices;
  if (!table_exists(conn, "tb_device")) return devices;
  // 旧库只保证「存在 tb_device」，列名按实际探测，缺列按 NULL 读取。
  const std::set<std::string> columns = table_columns(conn, "tb_device");
  Stmt stmt(conn, "SELECT " + column_or_null(columns, "dev_id") + ", " +
                      column_or_null(columns, "dev_name") + ", " +
                      column_or_null(columns, "location") + ", " +
                      column_or_null(columns, "control_range") + ", " +
                      column_or_null(columns, "phone") + " FROM tb_device" +
                      (columns.count("dev_id") ? " ORDER BY dev_id" : ""));
  while (stmt.step()) {
    LegacyDevice device;
    device.id = stmt.is_null(0) ? 0 : stmt.i64(0);
    device.name = stmt.text(1);
    device.location = stmt.text(2);
    device.control_range = stmt.text(3);
    device.phone = stmt.text(4);
    devices.push_back(std::move(device));
  }
  return devices;
}

std::vector<LegacyUser> read_users(sqlite3* conn) {
  std::vector<LegacyUser> users;
  if (!table_exists(conn, "tb_user")) return users;
  const std::set<std::string> columns = table_columns(conn, "tb_user");
  Stmt stmt(conn, "SELECT " + column_or_null(columns, "userName") + ", " +
                      column_or_null(columns, "userPwd") + " FROM tb_user" +
                      (columns.count("userName") ? " ORDER BY userName" : ""));
  while (stmt.step()) {
    LegacyUser user;
    user.name = stmt.text(0);
    user.password = stmt.text(1);
    users.push_back(std::move(user));
  }
  return users;
}

bool device_id_exists(sqlite3* conn, std::int64_t id) {
  Stmt stmt(conn, "SELECT 1 FROM device WHERE id = ? LIMIT 1");
  stmt.bind(1, id);
  return stmt.step();
}

bool same_device(sqlite3* conn, std::int64_t id, const LegacyDevice& device) {
  Stmt stmt(conn,
            "SELECT dev_name, location, control_range, phone FROM device"
            " WHERE id = ?");
  stmt.bind(1, id);
  if (!stmt.step()) return false;
  return stmt.text(0) == device.name && stmt.text(1) == device.location &&
         stmt.text(2) == device.control_range && stmt.text(3) == device.phone;
}

void insert_device(sqlite3* conn, const LegacyDevice& device,
                   bool keep_id) {
  const std::string now = now_text();
  Stmt stmt(conn,
            keep_id
                ? "INSERT INTO device(id,dev_name,location,control_range,phone,"
                  "created_at,updated_at) VALUES(?,?,?,?,?,?,?)"
                : "INSERT INTO device(dev_name,location,control_range,phone,"
                  "created_at,updated_at) VALUES(?,?,?,?,?,?)");
  int index = 1;
  if (keep_id) stmt.bind(index++, device.id);
  stmt.bind(index++, device.name)
      .bind(index++, device.location)
      .bind(index++, device.control_range)
      .bind(index++, device.phone)
      .bind(index++, now)
      .bind(index++, now);
  stmt.run();
}

bool is_default_user(const std::string& name) {
  return name == "root" || name == "安业物业";
}

std::string archive_name(const std::filesystem::path& source) {
  return source.filename().string() + ".migrated-" + stamp();
}

}  // namespace

LegacyInfo inspect_legacy(const std::filesystem::path& file) {
  LegacyInfo info;
  std::error_code ec;
  if (!std::filesystem::exists(file, ec) || !std::filesystem::is_regular_file(file, ec)) {
    return info;
  }
  SqlitePtr conn = open_readonly(file);
  const bool has_marker = table_exists(conn.get(), "schema_migration");
  const bool has_device = table_exists(conn.get(), "tb_device");
  const bool has_user = table_exists(conn.get(), "tb_user");
  info.is_legacy = !has_marker && (has_device || has_user);
  if (info.is_legacy) {
    info.devices = count_rows(conn.get(), "tb_device");
    info.users = count_rows(conn.get(), "tb_user");
  }
  return info;
}

LegacyImportReport import_legacy(const std::filesystem::path& source,
                                 const std::filesystem::path& data_dir) {
  const LegacyInfo info = inspect_legacy(source);
  if (!info.is_legacy) {
    throw AppError("not_legacy",
                   "不是旧版数据库（缺少 tb_device / tb_user，或已有 schema 版本）: " +
                       path_utf8(source));
  }

  LegacyImportReport report;
  report.source = path_utf8(source);
  report.devices_total = info.devices;
  report.users_total = info.users;

  // 1. 先备份并校验：备份不可信就停，绝不在原库上做任何写操作。
  //    WAL 模式旧库的已提交事务可能还在 -wal 里，必须一起复制。
  const std::filesystem::path backup_dir = data_dir / "backups";
  std::error_code ec;
  std::filesystem::create_directories(backup_dir, ec);
  if (ec) {
    throw AppError("backup_failed", "无法创建备份目录: " + ec.message());
  }
  std::filesystem::path backup =
      backup_dir / ("devicedb-legacy-" + stamp() + ".db");
  int suffix = 2;
  while (std::filesystem::exists(backup, ec)) {
    backup = backup_dir / ("devicedb-legacy-" + stamp() + "-" +
                           std::to_string(suffix++) + ".db");
  }
  std::filesystem::copy_file(source, backup,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    throw AppError("backup_failed", "旧库备份失败: " + ec.message());
  }
  const std::filesystem::path source_wal = source.string() + "-wal";
  const std::filesystem::path backup_wal = backup.string() + "-wal";
  const bool has_wal =
      std::filesystem::exists(source_wal, ec) &&
      std::filesystem::file_size(source_wal, ec) > 0;
  if (has_wal) {
    std::filesystem::copy_file(source_wal, backup_wal,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      throw AppError("backup_failed", "旧库 WAL 备份失败: " + ec.message());
    }
    if (sha256_file(source_wal) != sha256_file(backup_wal)) {
      throw AppError("backup_failed", "旧库 WAL 备份校验失败（SHA256 不一致）");
    }
  }
  report.source_sha256 = sha256_file(source);
  if (sha256_file(backup) != report.source_sha256) {
    std::filesystem::remove(backup, ec);
    throw AppError("backup_failed", "旧库备份校验失败（SHA256 不一致）");
  }
  report.backup = path_utf8(backup);

  // 2. 只从校验过的备份读取数据；旧库文件本身不再打开。
  std::vector<LegacyDevice> devices;
  std::vector<LegacyUser> users;
  {
    SqlitePtr conn = open_readonly(backup);
    {
      Stmt stmt(conn.get(), "PRAGMA integrity_check");
      const std::string result = stmt.step() ? stmt.text(0) : std::string();
      if (result != "ok") {
        throw AppError("backup_failed", "旧库备份完整性检查失败: " + result);
      }
    }
    if (count_rows(conn.get(), "tb_device") != info.devices ||
        count_rows(conn.get(), "tb_user") != info.users) {
      throw AppError("backup_failed",
                     "旧库备份行数与源库不一致（WAL 可能未完整复制）");
    }
    devices = read_devices(conn.get());
    users = read_users(conn.get());
  }

  const std::filesystem::path target = data_dir / "devicedb.db";
  bool import_in_place = false;
  bool target_is_legacy = false;
  if (std::filesystem::exists(target, ec)) {
    LegacyInfo target_info = inspect_legacy(target);
    if (target_info.is_legacy) {
      target_is_legacy = true;
    } else {
      import_in_place = true;
    }
  }
  const std::filesystem::path work =
      import_in_place ? target : data_dir / "devicedb.importing.db";
  if (!import_in_place) {
    std::filesystem::remove(work, ec);
    std::filesystem::remove(work.string() + "-wal", ec);
    std::filesystem::remove(work.string() + "-shm", ec);
  }

  {
    Db db(work);
    migrate(db);

    db.write([&](sqlite3* conn) {
      for (const LegacyDevice& raw : devices) {
        LegacyDevice device = raw;
        device.name = trim(device.name);
        device.location = trim(device.location);
        device.control_range = trim(device.control_range);
        device.phone = trim(device.phone);
        bool truncated = false;
        for (std::string* field : {&device.name, &device.location,
                                   &device.control_range, &device.phone}) {
          const std::string cut = truncate_utf8(*field, 20);
          if (cut != *field) truncated = true;
          *field = cut;
        }
        if (truncated) {
          report.warnings.push_back("dev_id=" + std::to_string(raw.id) +
                                    " 有字段超过 20 字符，已截断");
        }
        if (device.name.empty() || device.location.empty() ||
            device.control_range.empty()) {
          ++report.devices_skipped;
          report.warnings.push_back(
              "dev_id=" + std::to_string(raw.id) +
              " 缺少设备名称/位置/控制范围，已跳过（旧库原值保留在备份中）");
          continue;
        }
        if (raw.id > 0 && device_id_exists(conn, raw.id)) {
          if (same_device(conn, raw.id, device)) {
            ++report.devices_skipped;
          } else {
            insert_device(conn, device, false);
            ++report.devices_imported;
            report.warnings.push_back(
                "dev_id=" + std::to_string(raw.id) +
                " 与目标库现有 id 冲突，已顺延为新 id");
          }
          continue;
        }
        insert_device(conn, device, raw.id > 0);
        ++report.devices_imported;
      }

      for (const LegacyUser& raw : users) {
        std::string name = truncate_utf8(trim(raw.name), 20);
        if (name.empty()) {
          ++report.users_skipped;
          report.warnings.push_back("旧库存在空用户名，已跳过");
          continue;
        }
        Stmt exists(conn, "SELECT 1 FROM app_user WHERE user_name = ? LIMIT 1");
        exists.bind(1, name);
        if (exists.step()) {
          ++report.users_skipped;
          continue;
        }
        std::string plain = raw.password;
        bool generated = false;
        if (plain.empty()) {
          plain = random_password();
          generated = true;
        }
        User user;
        user.user_name = name;
        user.display_name = name;
        user.role = name == "root" ? "admin" : "user";
        user.password_hash = hash_password(plain);
        user.must_change_password =
            generated || is_default_user(name) || utf8_length(plain) < 6;
        Stmt insert(conn,
                    "INSERT INTO app_user(user_name,password_hash,display_name,"
                    "role,must_change_password,failed_count,locked_until,"
                    "created_at,updated_at) VALUES(?,?,?,?,?,0,NULL,?,?)");
        const std::string now = now_text();
        insert.bind(1, user.user_name)
            .bind(2, user.password_hash)
            .bind(3, user.display_name)
            .bind(4, user.role)
            .bind(5, static_cast<std::int64_t>(user.must_change_password ? 1 : 0))
            .bind(6, now)
            .bind(7, now);
        insert.run();
        ++report.users_imported;
        if (generated) {
          report.warnings.push_back("用户 " + name +
                                    " 旧口令为空，已生成随机口令并强制改密");
        }
      }

      set_setting(conn, "legacy_imported_from", report.source);
      set_setting(conn, "legacy_imported_at", now_text());
      set_setting(conn, "legacy_source_sha256", report.source_sha256);
    });
  }

  // 3. 旧库归档：文件名加 .migrated-<时间戳>，不删除、不覆盖。
  if (!import_in_place) {
    if (target_is_legacy && std::filesystem::exists(target, ec)) {
      const std::filesystem::path archived = target.parent_path() / archive_name(target);
      std::filesystem::rename(target, archived, ec);
      if (ec) {
        throw AppError("legacy_archive",
                       "新库已导入，但旧库重命名失败: " + ec.message() +
                           "（备份在 " + backup.string() + "）");
      }
      for (const char* suffix : {"-wal", "-shm"}) {
        const std::filesystem::path sidecar = target.string() + suffix;
        std::error_code sidecar_ec;
        if (std::filesystem::exists(sidecar, sidecar_ec)) {
          std::filesystem::rename(sidecar, archived.string() + suffix, sidecar_ec);
        }
      }
      report.archived_as = path_utf8(archived);
    }
    std::filesystem::rename(work, target, ec);
    if (ec) {
      throw AppError("legacy_swap", "导入完成但切换新库失败: " + ec.message() +
                                        "（备份在 " + backup.string() + "）");
    }
  }

  report.performed = true;
  return report;
}

bool needs_legacy_import(const std::filesystem::path& data_dir) {
  std::error_code ec;
  const std::filesystem::path main_db = data_dir / "devicedb.db";
  if (std::filesystem::exists(main_db, ec) && inspect_legacy(main_db).is_legacy) {
    return true;
  }
  const std::filesystem::path legacy = data_dir / "legacy" / "devicedb.db";
  return std::filesystem::exists(legacy, ec) && inspect_legacy(legacy).is_legacy;
}

LegacyImportReport auto_import_if_needed(const std::filesystem::path& data_dir) {
  std::error_code ec;
  const std::filesystem::path main_db = data_dir / "devicedb.db";
  if (std::filesystem::exists(main_db, ec) && inspect_legacy(main_db).is_legacy) {
    return import_legacy(main_db, data_dir);
  }
  const std::filesystem::path legacy = data_dir / "legacy" / "devicedb.db";
  if (std::filesystem::exists(legacy, ec) && inspect_legacy(legacy).is_legacy) {
    return import_legacy(legacy, data_dir);
  }
  return {};
}

}  // namespace devcard
