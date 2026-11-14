#include "devcard/settings.h"

namespace devcard {

std::string get_setting(const Db& db, std::string_view key) {
  std::string value;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT value FROM app_setting WHERE key = ?");
    stmt.bind(1, key);
    if (stmt.step()) value = stmt.text(0);
  });
  return value;
}

bool has_setting(const Db& db, std::string_view key) {
  bool found = false;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT 1 FROM app_setting WHERE key = ? LIMIT 1");
    stmt.bind(1, key);
    found = stmt.step();
  });
  return found;
}

void set_setting(sqlite3* conn, std::string_view key, std::string_view value) {
  Stmt stmt(conn,
            "INSERT INTO app_setting(key,value) VALUES(?,?)"
            " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  stmt.bind(1, key).bind(2, value);
  stmt.run();
}

}  // namespace devcard
