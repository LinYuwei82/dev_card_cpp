#include "devcard/dao/setting_dao.h"

namespace devcard {

std::string SettingDao::get(const Db& db, std::string_view key) const {
  std::string value;
  db.read([&](Conn& conn) {
    Stmt stmt(conn, "SELECT value FROM app_setting WHERE key = ?");
    stmt.bind(1, key);
    if (stmt.step()) value = stmt.text(0);
  });
  return value;
}

bool SettingDao::has(const Db& db, std::string_view key) const {
  bool found = false;
  db.read([&](Conn& conn) {
    Stmt stmt(conn, "SELECT 1 FROM app_setting WHERE key = ? LIMIT 1");
    stmt.bind(1, key);
    found = stmt.step();
  });
  return found;
}

void SettingDao::set(Conn& conn, std::string_view key,
                     std::string_view value) const {
  Stmt stmt(conn,
            "INSERT INTO app_setting(key,value) VALUES(?,?)"
            " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  stmt.bind(1, key).bind(2, value);
  stmt.run();
}

}  // namespace devcard
