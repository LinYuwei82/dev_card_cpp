#include "devcard/migrations.h"

#include <string>

#include "devcard/error.h"
#include "sha256.h"

namespace devcard {
namespace {

constexpr const char* kMigration001 = R"sql(
CREATE TABLE device (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  dev_name      TEXT    NOT NULL,
  location      TEXT    NOT NULL,
  control_range TEXT    NOT NULL,
  phone         TEXT    NOT NULL DEFAULT '',
  created_at    TEXT    NOT NULL,
  updated_at    TEXT    NOT NULL,
  CHECK (length(dev_name) BETWEEN 1 AND 20),
  CHECK (length(location) BETWEEN 1 AND 20),
  CHECK (length(control_range) BETWEEN 1 AND 20),
  CHECK (length(phone) <= 20)
);
CREATE INDEX idx_device_name ON device(dev_name);
CREATE INDEX idx_device_location ON device(location);
CREATE INDEX idx_device_triplet ON device(dev_name, location, control_range);

CREATE TABLE app_user (
  user_name            TEXT PRIMARY KEY,
  password_hash        TEXT NOT NULL,
  display_name         TEXT NOT NULL DEFAULT '',
  role                 TEXT NOT NULL DEFAULT 'admin',
  must_change_password INTEGER NOT NULL DEFAULT 0,
  failed_count         INTEGER NOT NULL DEFAULT 0,
  locked_until         TEXT,
  created_at           TEXT NOT NULL,
  updated_at           TEXT NOT NULL,
  CHECK (role IN ('admin','user'))
);

CREATE TABLE app_setting (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
)sql";

const std::vector<Migration>& migrations() {
  static const std::vector<Migration> list = {
      {1, "initial_schema", kMigration001},
  };
  return list;
}

}  // namespace

const std::vector<Migration>& all_migrations() { return migrations(); }

int target_schema_version() {
  return migrations().empty() ? 0 : migrations().back().version;
}

void migrate(Db& db) {
  db.write([&](sqlite3* conn) {
    exec_sql(conn,
             "CREATE TABLE IF NOT EXISTS schema_migration ("
             "version INTEGER PRIMARY KEY,"
             "name TEXT NOT NULL,"
             "checksum TEXT NOT NULL,"
             "applied_at TEXT NOT NULL)");

    std::vector<int> applied;
    {
      Stmt stmt(conn, "SELECT version FROM schema_migration ORDER BY version");
      while (stmt.step()) applied.push_back(static_cast<int>(stmt.i64(0)));
    }

    const int target = target_schema_version();
    if (!applied.empty() && applied.back() > target) {
      throw AppError("schema_newer",
                     "数据库版本高于程序支持的版本，请升级程序");
    }

    for (const Migration& migration : migrations()) {
      const std::string checksum = sha256_hex(migration.sql);
      bool done = false;
      {
        Stmt stmt(conn,
                  "SELECT checksum FROM schema_migration WHERE version = ?");
        stmt.bind(1, migration.version);
        if (stmt.step()) {
          done = true;
          if (stmt.text(0) != checksum) {
            throw AppError("migration_checksum",
                           "迁移 " + std::to_string(migration.version) +
                               " 的校验和与已应用记录不一致");
          }
        }
      }
      if (done) continue;

      exec_sql(conn, migration.sql);
      Stmt insert(conn,
                  "INSERT INTO schema_migration(version,name,checksum,applied_at)"
                  " VALUES(?,?,?,?)");
      insert.bind(1, migration.version)
          .bind(2, migration.name)
          .bind(3, checksum)
          .bind(4, now_text());
      insert.run();
    }

    exec_sql(conn, std::string("PRAGMA user_version = ") + std::to_string(target));
  });
}

}  // namespace devcard
