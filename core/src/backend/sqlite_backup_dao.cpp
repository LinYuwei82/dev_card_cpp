// BackupDao 的 SQLite 实现：VACUUM INTO / ATTACH / PRAGMA 集中于此。
#include "backend/sqlite_backup_dao.h"

#include <sqlite3.h>

#include <memory>
#include <string>

#include "backend/sqlite_conn.h"
#include "devcard/db.h"
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

}  // namespace

void SqliteBackupDao::vacuum_into(Db& db, const std::string& target_utf8) const {
  db.write_raw([&](Conn& conn) {
    Stmt stmt(conn, "VACUUM INTO ?");
    stmt.bind(1, target_utf8);
    stmt.run();
  });
}

void SqliteBackupDao::verify_backup_file(const std::filesystem::path& file) const {
  SqlitePtr raw = open_readonly(file);
  SqliteConn conn(raw.get());
  for (const char* table :
       {"device", "app_user", "app_setting", "schema_migration"}) {
    if (!conn.table_exists(table)) {
      throw AppError("bad_backup", "备份缺少表 " + std::string(table));
    }
  }
  Stmt stmt(conn, "PRAGMA integrity_check");
  const std::string result = stmt.step() ? stmt.text(0) : std::string();
  if (result != "ok") {
    throw AppError("bad_backup", "备份完整性检查失败: " + result);
  }
}

void SqliteBackupDao::restore_from(Db& db, const std::string& source_utf8) const {
  // ATTACH/DETACH 不能在事务内执行：先挂载，再开写事务，提交后卸载。
  db.write_raw([&](Conn& conn) {
    {
      Stmt attach(conn, "ATTACH DATABASE ? AS restore_src");
      attach.bind(1, source_utf8);
      attach.run();
    }
    try {
      conn.exec("BEGIN IMMEDIATE");
      conn.exec("DELETE FROM device");
      // M7 之前的备份没有 version/updated_by 列：有就一并恢复，没有则用列默认值，
      // 否则恢复后所有记录 version 归 1，乐观锁会退化成「谁都能覆盖」。
      bool keep_locking = false;
      {
        Stmt columns(conn, "PRAGMA restore_src.table_info(device)");
        bool has_version = false;
        bool has_updated_by = false;
        while (columns.step()) {
          const std::string name = columns.text(1);
          if (name == "version") has_version = true;
          if (name == "updated_by") has_updated_by = true;
        }
        keep_locking = has_version && has_updated_by;
      }
      if (keep_locking) {
        conn.exec(
            "INSERT INTO device(id,dev_name,location,control_range,phone,"
            "created_at,updated_at,version,updated_by) SELECT id,dev_name,"
            "location,control_range,phone,created_at,updated_at,version,"
            "updated_by FROM restore_src.device");
      } else {
        conn.exec(
            "INSERT INTO device(id,dev_name,location,control_range,phone,"
            "created_at,updated_at) SELECT id,dev_name,location,"
            "control_range,phone,created_at,updated_at FROM restore_src.device");
      }
      conn.exec("DELETE FROM app_user");
      conn.exec(
          "INSERT INTO app_user(user_name,password_hash,display_name,role,"
          "must_change_password,failed_count,locked_until,created_at,"
          "updated_at) SELECT user_name,password_hash,display_name,role,"
          "must_change_password,failed_count,locked_until,created_at,"
          "updated_at FROM restore_src.app_user");
      conn.exec("DELETE FROM app_setting");
      conn.exec(
          "INSERT INTO app_setting(key,value) SELECT key,value"
          " FROM restore_src.app_setting");
      conn.exec("COMMIT");
    } catch (...) {
      try {
        conn.exec("ROLLBACK");
      } catch (...) {
      }
      try {
        conn.exec("DETACH DATABASE restore_src");
      } catch (...) {
      }
      throw;
    }
    conn.exec("DETACH DATABASE restore_src");
  });
}

}  // namespace devcard
