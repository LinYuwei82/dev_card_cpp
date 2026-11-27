#pragma once

#include <sqlite3.h>

#include "devcard/db.h"

namespace devcard {

// Conn 的 SQLite 实现：薄封装一个已打开的 sqlite3* 连接。
// 供 SqliteDb 与 SqliteBackupDao（独立打开备份文件连接）共用；
// 仅 core/src/backend/ 内部可见，对外只暴露 devcard/backend/sqlite_db.h 的工厂。
class SqliteConn final : public Conn {
 public:
  explicit SqliteConn(sqlite3* db = nullptr) : db_(db) {}

  std::unique_ptr<StmtView> prepare(std::string_view sql) override;
  void exec(std::string_view sql) override;
  bool table_exists(std::string_view name) override;
  int changes() const override;

  sqlite3* raw() const { return db_; }

 private:
  sqlite3* db_ = nullptr;
};

}  // namespace devcard
