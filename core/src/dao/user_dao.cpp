#include "devcard/dao/user_dao.h"

#include <string>

namespace devcard {
namespace {

const char* kColumns =
    "user_name, password_hash, display_name, role, must_change_password, "
    "failed_count, locked_until, created_at, updated_at";

}  // namespace

void UserDao::read_row(Stmt& stmt, User& user) {
  user.user_name = stmt.text(0);
  user.password_hash = stmt.text(1);
  user.display_name = stmt.text(2);
  user.role = stmt.text(3);
  user.must_change_password = stmt.i64(4) != 0;
  user.failed_count = static_cast<int>(stmt.i64(5));
  user.locked_until = stmt.is_null(6) ? std::string() : stmt.text(6);
  user.created_at = stmt.text(7);
  user.updated_at = stmt.text(8);
}

std::vector<User> UserDao::list(const Db& db) const {
  std::vector<User> users;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, std::string("SELECT ") + kColumns +
                        " FROM app_user ORDER BY user_name");
    while (stmt.step()) {
      User user;
      read_row(stmt, user);
      users.push_back(std::move(user));
    }
  });
  return users;
}

std::int64_t UserDao::count(const Db& db) const {
  std::int64_t total = 0;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT COUNT(*) FROM app_user");
    if (stmt.step()) total = stmt.i64(0);
  });
  return total;
}

bool UserDao::find(const Db& db, std::string_view name, User& out) const {
  bool found = false;
  db.read([&](sqlite3* conn) { found = find(conn, name, out); });
  return found;
}

bool UserDao::find(sqlite3* conn, std::string_view name, User& out) const {
  Stmt stmt(conn, std::string("SELECT ") + kColumns +
                      " FROM app_user WHERE user_name = ?");
  stmt.bind(1, name);
  if (!stmt.step()) return false;
  read_row(stmt, out);
  return true;
}

void UserDao::insert(sqlite3* conn, const User& user) const {
  Stmt stmt(conn,
            "INSERT INTO app_user(user_name,password_hash,display_name,role,"
            "must_change_password,failed_count,locked_until,created_at,updated_at)"
            " VALUES(?,?,?,?,?,0,NULL,?,?)");
  const std::string now = now_text();
  stmt.bind(1, user.user_name)
      .bind(2, user.password_hash)
      .bind(3, user.display_name)
      .bind(4, user.role)
      .bind(5, static_cast<std::int64_t>(user.must_change_password ? 1 : 0))
      .bind(6, user.created_at.empty() ? now : user.created_at)
      .bind(7, now);
  stmt.run();
}

bool UserDao::exists(sqlite3* conn, std::string_view name) const {
  Stmt stmt(conn, "SELECT 1 FROM app_user WHERE user_name = ? LIMIT 1");
  stmt.bind(1, name);
  return stmt.step();
}

void UserDao::update_profile(sqlite3* conn, std::string_view name,
                             std::string_view display_name,
                             std::string_view role) const {
  Stmt stmt(conn,
            "UPDATE app_user SET display_name = ?, role = ?, updated_at = ?"
            " WHERE user_name = ?");
  stmt.bind(1, display_name).bind(2, role).bind(3, now_text()).bind(4, name);
  stmt.run();
}

void UserDao::set_password(sqlite3* conn, std::string_view name,
                           std::string_view password_hash,
                           bool must_change) const {
  Stmt stmt(conn,
            "UPDATE app_user SET password_hash = ?, must_change_password = ?,"
            " failed_count = 0, locked_until = NULL, updated_at = ?"
            " WHERE user_name = ?");
  stmt.bind(1, password_hash)
      .bind(2, static_cast<std::int64_t>(must_change ? 1 : 0))
      .bind(3, now_text())
      .bind(4, name);
  stmt.run();
}

void UserDao::record_failed_login(sqlite3* conn, std::string_view name,
                                  int failed_count,
                                  std::string_view locked_until) const {
  Stmt stmt(conn,
            "UPDATE app_user SET failed_count = ?, locked_until = ?,"
            " updated_at = ? WHERE user_name = ?");
  stmt.bind(1, static_cast<std::int64_t>(failed_count));
  if (locked_until.empty()) {
    stmt.bind_null(2);
  } else {
    stmt.bind(2, locked_until);
  }
  stmt.bind(3, now_text()).bind(4, name);
  stmt.run();
}

void UserDao::clear_failed_login(sqlite3* conn, std::string_view name) const {
  Stmt stmt(conn,
            "UPDATE app_user SET failed_count = 0, locked_until = NULL,"
            " updated_at = ? WHERE user_name = ?");
  stmt.bind(1, now_text()).bind(2, name);
  stmt.run();
}

void UserDao::erase(sqlite3* conn, std::string_view name) const {
  Stmt stmt(conn, "DELETE FROM app_user WHERE user_name = ?");
  stmt.bind(1, name);
  stmt.run();
}

std::int64_t UserDao::count_admins(sqlite3* conn) const {
  std::int64_t total = 0;
  Stmt stmt(conn, "SELECT COUNT(*) FROM app_user WHERE role = 'admin'");
  if (stmt.step()) total = stmt.i64(0);
  return total;
}

}  // namespace devcard
