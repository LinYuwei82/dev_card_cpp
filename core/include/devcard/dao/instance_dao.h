#pragma once

#include <string>
#include <string_view>

#include <sqlite3.h>

namespace devcard {

// instance_heartbeat 单行（id=1）的属主信息。
struct InstanceOwner {
  std::string instance_id;
  long long pid = 0;
  std::string host;
  std::string listen;
  std::string started_at;
  std::string heartbeat_at;
};

// instance_heartbeat 表 DAO：跨实例互斥的 SQL 只出现在本类实现中。
class InstanceDao {
 public:
  bool read(sqlite3* conn, InstanceOwner& owner) const;
  void write(sqlite3* conn, std::string_view instance_id, long long pid,
             std::string_view host, std::string_view started_at,
             std::string_view heartbeat_at) const;
  // 只有 instance_id 仍是自己时才刷新；返回 false 表示已被别的实例接管。
  bool touch(sqlite3* conn, std::string_view instance_id,
             std::string_view heartbeat_at) const;
  void set_listen(sqlite3* conn, std::string_view instance_id,
                  std::string_view listen) const;
  void erase(sqlite3* conn, std::string_view instance_id) const;
};

}  // namespace devcard
