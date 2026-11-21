#include "devcard/dao/instance_dao.h"

#include "devcard/db.h"

namespace devcard {
namespace {

constexpr int kOwnerId = 1;

}  // namespace

bool InstanceDao::read(sqlite3* conn, InstanceOwner& owner) const {
  Stmt stmt(conn,
            "SELECT instance_id,pid,host,listen,started_at,heartbeat_at"
            " FROM instance_heartbeat WHERE id = ?");
  stmt.bind(1, static_cast<std::int64_t>(kOwnerId));
  if (!stmt.step()) return false;
  owner.instance_id = stmt.text(0);
  owner.pid = stmt.i64(1);
  owner.host = stmt.text(2);
  owner.listen = stmt.text(3);
  owner.started_at = stmt.text(4);
  owner.heartbeat_at = stmt.text(5);
  return true;
}

void InstanceDao::write(sqlite3* conn, std::string_view instance_id,
                        long long pid, std::string_view host,
                        std::string_view started_at,
                        std::string_view heartbeat_at) const {
  Stmt stmt(conn,
            "INSERT OR REPLACE INTO instance_heartbeat"
            "(id,instance_id,pid,host,listen,started_at,heartbeat_at)"
            " VALUES(?,?,?,?,'',?,?)");
  stmt.bind(1, static_cast<std::int64_t>(kOwnerId))
      .bind(2, instance_id)
      .bind(3, pid)
      .bind(4, host)
      .bind(5, started_at)
      .bind(6, heartbeat_at);
  stmt.run();
}

bool InstanceDao::touch(sqlite3* conn, std::string_view instance_id,
                        std::string_view heartbeat_at) const {
  Stmt stmt(conn,
            "UPDATE instance_heartbeat SET heartbeat_at = ?"
            " WHERE id = ? AND instance_id = ?");
  stmt.bind(1, heartbeat_at)
      .bind(2, static_cast<std::int64_t>(kOwnerId))
      .bind(3, instance_id);
  stmt.run();
  return sqlite3_changes(conn) > 0;
}

void InstanceDao::set_listen(sqlite3* conn, std::string_view instance_id,
                             std::string_view listen) const {
  Stmt stmt(conn,
            "UPDATE instance_heartbeat SET listen = ?"
            " WHERE id = ? AND instance_id = ?");
  stmt.bind(1, listen)
      .bind(2, static_cast<std::int64_t>(kOwnerId))
      .bind(3, instance_id);
  stmt.run();
}

void InstanceDao::erase(sqlite3* conn, std::string_view instance_id) const {
  Stmt stmt(conn,
            "DELETE FROM instance_heartbeat WHERE id = ? AND instance_id = ?");
  stmt.bind(1, static_cast<std::int64_t>(kOwnerId)).bind(2, instance_id);
  stmt.run();
}

}  // namespace devcard
