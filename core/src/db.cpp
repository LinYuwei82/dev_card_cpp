// 存储抽象层的后端无关部分：Stmt 薄包装、exec_sql 兼容入口、now_text。
// SQLite 实现见 backend/sqlite_db.cpp。
#include "devcard/db.h"

#include <chrono>
#include <ctime>
#include <utility>

namespace devcard {

Stmt::Stmt(Conn& conn, std::string_view sql) : view_(conn.prepare(sql)) {}
Stmt::~Stmt() = default;

Stmt& Stmt::bind(std::int64_t index, std::int64_t value) {
  view_->bind(index, value);
  return *this;
}

Stmt& Stmt::bind(std::int64_t index, std::string_view value) {
  view_->bind(index, value);
  return *this;
}

Stmt& Stmt::bind_null(std::int64_t index) {
  view_->bind_null(index);
  return *this;
}

bool Stmt::step() { return view_->step(); }
void Stmt::run() { view_->run(); }

std::int64_t Stmt::i64(int column) const { return view_->i64(column); }
std::string Stmt::text(int column) const { return view_->text(column); }
bool Stmt::is_null(int column) const { return view_->is_null(column); }
std::int64_t Stmt::last_insert_id() const { return view_->last_insert_id(); }

void exec_sql(Conn& conn, std::string_view sql) { conn.exec(sql); }

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
