#pragma once

#include <sqlite3.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace devcard {

// sqlite3_stmt 的 RAII 薄封装：只做绑定、单步与取值，不含业务语义。
class Stmt {
 public:
  Stmt(sqlite3* db, std::string_view sql);
  ~Stmt();

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bind(std::int64_t index, std::int64_t value);
  Stmt& bind(std::int64_t index, std::string_view value);
  Stmt& bind_null(std::int64_t index);

  bool step();
  void run();

  std::int64_t i64(int column) const;
  std::string text(int column) const;
  bool is_null(int column) const;

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

// 连接管理：单写连接 + 小读池。事务只在 write()/read() 边界开启。
class Db {
 public:
  explicit Db(std::filesystem::path file, int read_connections = 2);
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  void write(const std::function<void(sqlite3*)>& fn);
  void write_raw(const std::function<void(sqlite3*)>& fn);
  void read(const std::function<void(sqlite3*)>& fn) const;

  std::int64_t schema_version() const;
  const std::filesystem::path& file() const { return file_; }

 private:
  std::filesystem::path file_;
  sqlite3* write_ = nullptr;
  std::mutex write_mutex_;

  mutable std::mutex pool_mutex_;
  mutable std::condition_variable pool_cv_;
  mutable std::vector<sqlite3*> reads_;
  mutable std::vector<bool> read_busy_;
};

void exec_sql(sqlite3* db, std::string_view sql);

std::string now_text();

}  // namespace devcard
