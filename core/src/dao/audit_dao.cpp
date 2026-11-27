#include "devcard/dao/audit_dao.h"

#include <algorithm>

namespace devcard {

void AuditDao::insert(Conn& conn, const AuditEntry& entry,
                      std::string_view at) const {
  Stmt stmt(conn,
            "INSERT INTO audit_log(at,actor,actor_ip,action,entity,entity_id,"
            "summary,before_json,after_json) VALUES(?,?,?,?,?,?,?,?,?)");
  stmt.bind(1, at)
      .bind(2, entry.actor)
      .bind(3, entry.actor_ip)
      .bind(4, entry.action)
      .bind(5, entry.entity)
      .bind(6, entry.entity_id)
      .bind(7, entry.summary)
      .bind(8, entry.before_json)
      .bind(9, entry.after_json);
  stmt.run();
}

std::vector<AuditEntry> AuditDao::list(const Db& db, int limit) const {
  const int capped = std::clamp(limit, 1, 1000);
  std::vector<AuditEntry> entries;
  db.read([&](Conn& conn) {
    Stmt stmt(conn,
              "SELECT id,at,actor,actor_ip,action,entity,entity_id,summary,"
              "before_json,after_json FROM audit_log ORDER BY id DESC LIMIT ?");
    stmt.bind(1, static_cast<std::int64_t>(capped));
    while (stmt.step()) {
      AuditEntry entry;
      entry.id = stmt.i64(0);
      entry.at = stmt.text(1);
      entry.actor = stmt.text(2);
      entry.actor_ip = stmt.text(3);
      entry.action = stmt.text(4);
      entry.entity = stmt.text(5);
      entry.entity_id = stmt.text(6);
      entry.summary = stmt.text(7);
      entry.before_json = stmt.text(8);
      entry.after_json = stmt.text(9);
      entries.push_back(std::move(entry));
    }
  });
  return entries;
}

}  // namespace devcard
