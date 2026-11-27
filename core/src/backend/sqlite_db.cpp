// SQLite 后端实现：全仓库唯一允许直接使用 sqlite3 C API 的生产代码
// （备份的 VACUUM/ATTACH 见 sqlite_backup_dao.cpp，测试/压测允许裸用）。
#include "backend/sqlite_conn.h"

#include <sqlite3.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "backend/sqlite_backup_dao.h"
#include "devcard/backend/sqlite_db.h"
#include "devcard/error.h"
#include "devcard/paths.h"

namespace devcard {
namespace {

// 写争用的等待上限。正常并发（5–20 客户端）由 write_mutex_ 串行化，不会触发；
// 只有外部工具（DB 浏览器、备份脚本）持锁时才会等。超时后向上抛 locked，
// 由 HTTP 层映射成 423，让用户看到「数据库正被其他程序占用」，不无限重试。
constexpr int kBusyTimeoutMs = 5000;

[[noreturn]] void throw_sqlite(sqlite3* db, const char* what) {
  const char* message = db ? sqlite3_errmsg(db) : "sqlite error";
  throw AppError("db_error", std::string(what) + ": " + message);
}

sqlite3* open_connection(const std::string& file, int flags) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(file.c_str(), &db, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    if (db) sqlite3_close(db);
    throw AppError("db_open", "无法打开数据库 " + file + ": " + message);
  }
  sqlite3_busy_timeout(db, kBusyTimeoutMs);
  return db;
}

void exec_checked(sqlite3* db, const char* sql) {
  char* error = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    std::string message = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw AppError("db_exec", message);
  }
}

// 忽略错误的收尾执行（ROLLBACK/DETACH），与原实现行为一致。
void exec_quiet(sqlite3* db, const char* sql) {
  sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

class SqliteStmtView final : public StmtView {
 public:
  SqliteStmtView(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
  ~SqliteStmtView() override {
    if (stmt_) sqlite3_finalize(stmt_);
  }

  StmtView& bind(std::int64_t index, std::int64_t value) override {
    if (sqlite3_bind_int64(stmt_, static_cast<int>(index), value) != SQLITE_OK)
      throw_sqlite(db_, "bind int");
    return *this;
  }

  StmtView& bind(std::int64_t index, std::string_view value) override {
    if (sqlite3_bind_text(stmt_, static_cast<int>(index), value.data(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
      throw_sqlite(db_, "bind text");
    return *this;
  }

  StmtView& bind_null(std::int64_t index) override {
    if (sqlite3_bind_null(stmt_, static_cast<int>(index)) != SQLITE_OK)
      throw_sqlite(db_, "bind null");
    return *this;
  }

  bool step() override {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw_sqlite(db_, "step");
  }

  void run() override {
    if (step()) throw AppError("db_unexpected_row", "语句返回了意外结果集");
  }

  std::int64_t i64(int column) const override {
    return sqlite3_column_int64(stmt_, column);
  }

  std::string text(int column) const override {
    const unsigned char* value = sqlite3_column_text(stmt_, column);
    if (!value) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(sqlite3_column_bytes(stmt_, column)));
  }

  bool is_null(int column) const override {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
  }

  std::int64_t last_insert_id() const override {
    return sqlite3_last_insert_rowid(db_);
  }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace

std::unique_ptr<StmtView> SqliteConn::prepare(std::string_view sql) {
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()),
                                    &stmt, nullptr);
  if (rc != SQLITE_OK) throw_sqlite(db_, "prepare");
  return std::make_unique<SqliteStmtView>(db_, stmt);
}

void SqliteConn::exec(std::string_view sql) {
  exec_checked(db_, std::string(sql).c_str());
}

bool SqliteConn::table_exists(std::string_view name) {
  Stmt stmt(*this, "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?");
  stmt.bind(1, name);
  return stmt.step();
}

int SqliteConn::changes() const { return sqlite3_changes(db_); }

namespace {

class SqliteDb final : public Db {
 public:
  explicit SqliteDb(std::filesystem::path file, int read_connections);
  ~SqliteDb() override;

  void write(const std::function<void(Conn&)>& fn) override;
  void write_raw(const std::function<void(Conn&)>& fn) override;
  void read(const std::function<void(Conn&)>& fn) const override;
  void checkpoint() override;

  std::int64_t schema_version() const override;
  void set_schema_version(Conn& conn, int version) override;

  std::string backend_name() const override { return "sqlite"; }
  std::string describe() const override { return path_to_utf8(file_); }
  const BackupDao& backup() const override { return backup_; }

 private:
  std::filesystem::path file_;
  sqlite3* write_ = nullptr;
  std::unique_ptr<SqliteConn> write_conn_;
  std::mutex write_mutex_;

  mutable std::mutex pool_mutex_;
  mutable std::condition_variable pool_cv_;
  mutable std::vector<sqlite3*> reads_;
  mutable std::vector<bool> read_busy_;
  mutable std::vector<std::unique_ptr<SqliteConn>> read_conns_;

  SqliteBackupDao backup_;
};

SqliteDb::SqliteDb(std::filesystem::path file, int read_connections)
    : file_(std::move(file)) {
  read_connections = std::max(1, read_connections);
  std::error_code ec;
  if (!file_.parent_path().empty()) {
    std::filesystem::create_directories(file_.parent_path(), ec);
    if (ec) throw AppError("db_dir", "无法创建数据目录: " + ec.message());
  }

  const std::string path = path_to_utf8(file_);
  write_ = open_connection(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                     SQLITE_OPEN_FULLMUTEX);
  write_conn_ = std::make_unique<SqliteConn>(write_);
  exec_checked(write_, "PRAGMA journal_mode=WAL");
  exec_checked(write_, "PRAGMA synchronous=NORMAL");
  exec_checked(write_, "PRAGMA foreign_keys=ON");

  for (int i = 0; i < read_connections; ++i) {
    sqlite3* conn = open_connection(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    exec_checked(conn, "PRAGMA query_only=ON");
    reads_.push_back(conn);
    read_busy_.push_back(false);
    read_conns_.push_back(std::make_unique<SqliteConn>(conn));
  }
}

SqliteDb::~SqliteDb() {
  for (sqlite3* conn : reads_) sqlite3_close(conn);
  if (write_) sqlite3_close(write_);
}

void SqliteDb::write_raw(const std::function<void(Conn&)>& fn) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  fn(*write_conn_);
}

void SqliteDb::write(const std::function<void(Conn&)>& fn) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  exec_checked(write_, "BEGIN IMMEDIATE");
  try {
    fn(*write_conn_);
    exec_checked(write_, "COMMIT");
  } catch (...) {
    exec_quiet(write_, "ROLLBACK");
    throw;
  }
}

void SqliteDb::checkpoint() {
  std::lock_guard<std::mutex> lock(write_mutex_);
  // PASSIVE：能写多少写多少，遇到读者不阻塞，失败也不影响主流程。
  exec_quiet(write_, "PRAGMA wal_checkpoint(PASSIVE)");
}

void SqliteDb::read(const std::function<void(Conn&)>& fn) const {
  sqlite3* conn = nullptr;
  SqliteConn* iface = nullptr;
  std::size_t slot = 0;
  {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    pool_cv_.wait(lock, [&] {
      for (std::size_t i = 0; i < reads_.size(); ++i) {
        if (!read_busy_[i]) {
          slot = i;
          return true;
        }
      }
      return false;
    });
    read_busy_[slot] = true;
    conn = reads_[slot];
    iface = read_conns_[slot].get();
  }

  try {
    exec_checked(conn, "BEGIN");
    fn(*iface);
    exec_checked(conn, "COMMIT");
  } catch (...) {
    exec_quiet(conn, "ROLLBACK");
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      read_busy_[slot] = false;
    }
    pool_cv_.notify_one();
    throw;
  }

  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    read_busy_[slot] = false;
  }
  pool_cv_.notify_one();
}

std::int64_t SqliteDb::schema_version() const {
  std::int64_t version = 0;
  read([&](Conn& conn) {
    Stmt stmt(conn, "PRAGMA user_version");
    if (stmt.step()) version = stmt.i64(0);
  });
  return version;
}

void SqliteDb::set_schema_version(Conn& conn, int version) {
  conn.exec("PRAGMA user_version = " + std::to_string(version));
}

}  // namespace

std::unique_ptr<Db> open_sqlite_db(std::filesystem::path file, int read_connections) {
  return std::make_unique<SqliteDb>(std::move(file), read_connections);
}

}  // namespace devcard
