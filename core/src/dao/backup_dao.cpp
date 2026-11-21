#include "devcard/dao/backup_dao.h"

#include <string>

#include "devcard/db.h"
#include "devcard/error.h"

namespace devcard {
namespace {

bool table_exists(sqlite3* conn, const char* name) {
  Stmt stmt(conn, "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?");
  stmt.bind(1, name);
  return stmt.step();
}

}  // namespace

void BackupDao::vacuum_into(sqlite3* conn,
                            const std::string& target_utf8) const {
  Stmt stmt(conn, "VACUUM INTO ?");
  stmt.bind(1, target_utf8);
  stmt.run();
}

void BackupDao::verify_database(sqlite3* conn) const {
  for (const char* table :
       {"device", "app_user", "app_setting", "schema_migration"}) {
    if (!table_exists(conn, table)) {
      throw AppError("bad_backup", "备份缺少表 " + std::string(table));
    }
  }
  Stmt stmt(conn, "PRAGMA integrity_check");
  const std::string result = stmt.step() ? stmt.text(0) : std::string();
  if (result != "ok") {
    throw AppError("bad_backup", "备份完整性检查失败: " + result);
  }
}

void BackupDao::restore_from(sqlite3* conn,
                             const std::string& source_utf8) const {
  {
    Stmt attach(conn, "ATTACH DATABASE ? AS restore_src");
    attach.bind(1, source_utf8);
    attach.run();
  }
  try {
    exec_sql(conn, "BEGIN IMMEDIATE");
    exec_sql(conn, "DELETE FROM device");
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
      exec_sql(conn,
               "INSERT INTO device(id,dev_name,location,control_range,phone,"
               "created_at,updated_at,version,updated_by) SELECT id,dev_name,"
               "location,control_range,phone,created_at,updated_at,version,"
               "updated_by FROM restore_src.device");
    } else {
      exec_sql(conn,
               "INSERT INTO device(id,dev_name,location,control_range,phone,"
               "created_at,updated_at) SELECT id,dev_name,location,"
               "control_range,phone,created_at,updated_at FROM restore_src.device");
    }
    exec_sql(conn, "DELETE FROM app_user");
    exec_sql(conn,
             "INSERT INTO app_user(user_name,password_hash,display_name,role,"
             "must_change_password,failed_count,locked_until,created_at,"
             "updated_at) SELECT user_name,password_hash,display_name,role,"
             "must_change_password,failed_count,locked_until,created_at,"
             "updated_at FROM restore_src.app_user");
    exec_sql(conn, "DELETE FROM app_setting");
    exec_sql(conn,
             "INSERT INTO app_setting(key,value) SELECT key,value"
             " FROM restore_src.app_setting");
    exec_sql(conn, "COMMIT");
  } catch (...) {
    sqlite3_exec(conn, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "DETACH DATABASE restore_src", nullptr, nullptr, nullptr);
    throw;
  }
  exec_sql(conn, "DETACH DATABASE restore_src");
}

}  // namespace devcard
