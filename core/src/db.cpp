#include "devcard/db.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "devcard/error.h"

namespace devcard {
namespace {

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
  return path.string();
#endif
}

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
  sqlite3_busy_timeout(db, 5000);
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

}  // namespace

Stmt::Stmt(sqlite3* db, std::string_view sql) : db_(db) {
  const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()),
                                    &stmt_, nullptr);
  if (rc != SQLITE_OK) throw_sqlite(db, "prepare");
}

Stmt::~Stmt() {
  if (stmt_) sqlite3_finalize(stmt_);
}

Stmt& Stmt::bind(std::int64_t index, std::int64_t value) {
  if (sqlite3_bind_int64(stmt_, static_cast<int>(index), value) != SQLITE_OK)
    throw_sqlite(db_, "bind int");
  return *this;
}

Stmt& Stmt::bind(std::int64_t index, std::string_view value) {
  if (sqlite3_bind_text(stmt_, static_cast<int>(index), value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
    throw_sqlite(db_, "bind text");
  return *this;
}

Stmt& Stmt::bind_null(std::int64_t index) {
  if (sqlite3_bind_null(stmt_, static_cast<int>(index)) != SQLITE_OK)
    throw_sqlite(db_, "bind null");
  return *this;
}

bool Stmt::step() {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  throw_sqlite(db_, "step");
}

void Stmt::run() {
  if (step()) throw AppError("db_unexpected_row", "语句返回了意外结果集");
}

std::int64_t Stmt::i64(int column) const {
  return sqlite3_column_int64(stmt_, column);
}

std::string Stmt::text(int column) const {
  const unsigned char* value = sqlite3_column_text(stmt_, column);
  if (!value) return {};
  return std::string(reinterpret_cast<const char*>(value),
                     static_cast<std::size_t>(sqlite3_column_bytes(stmt_, column)));
}

bool Stmt::is_null(int column) const {
  return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

Db::Db(std::filesystem::path file, int read_connections) : file_(std::move(file)) {
  read_connections = std::max(1, read_connections);
  std::error_code ec;
  if (!file_.parent_path().empty()) {
    std::filesystem::create_directories(file_.parent_path(), ec);
    if (ec) throw AppError("db_dir", "无法创建数据目录: " + ec.message());
  }

  const std::string path = path_to_utf8(file_);
  write_ = open_connection(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                     SQLITE_OPEN_FULLMUTEX);
  exec_checked(write_, "PRAGMA journal_mode=WAL");
  exec_checked(write_, "PRAGMA synchronous=NORMAL");
  exec_checked(write_, "PRAGMA foreign_keys=ON");

  for (int i = 0; i < read_connections; ++i) {
    sqlite3* conn = open_connection(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    exec_checked(conn, "PRAGMA query_only=ON");
    reads_.push_back(conn);
    read_busy_.push_back(false);
  }
}

Db::~Db() {
  for (sqlite3* conn : reads_) sqlite3_close(conn);
  if (write_) sqlite3_close(write_);
}

void Db::write_raw(const std::function<void(sqlite3*)>& fn) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  fn(write_);
}

void Db::write(const std::function<void(sqlite3*)>& fn) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  exec_checked(write_, "BEGIN IMMEDIATE");
  try {
    fn(write_);
    exec_checked(write_, "COMMIT");
  } catch (...) {
    sqlite3_exec(write_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

void Db::read(const std::function<void(sqlite3*)>& fn) const {
  sqlite3* conn = nullptr;
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
  }

  try {
    exec_checked(conn, "BEGIN");
    fn(conn);
    exec_checked(conn, "COMMIT");
  } catch (...) {
    sqlite3_exec(conn, "ROLLBACK", nullptr, nullptr, nullptr);
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

std::int64_t Db::schema_version() const {
  std::int64_t version = 0;
  read([&](sqlite3* conn) {
    Stmt stmt(conn, "PRAGMA user_version");
    if (stmt.step()) version = stmt.i64(0);
  });
  return version;
}

void exec_sql(sqlite3* db, std::string_view sql) {
  char* error = nullptr;
  const int rc = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    std::string message = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw AppError("db_exec", message);
  }
}

std::string now_text() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
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

}  // namespace devcard
