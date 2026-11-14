#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "devcard/db.h"
#include "devcard/models.h"

namespace devcard {

// 设备台账 DAO：所有 device 表 SQL 只出现在本类实现中。
class DeviceDao {
 public:
  std::vector<Device> list(const Db& db, const DeviceQuery& query) const;
  std::int64_t count(const Db& db, const DeviceQuery& query) const;
  bool find(const Db& db, std::int64_t id, Device& out) const;
  // 写事务内的当前值读取（审计 before、存在性判断），与更新同一次提交。
  bool find(sqlite3* conn, std::int64_t id, Device& out) const;

  // 写方法要求调用方已处于 Db::write() 事务内。
  std::int64_t insert(sqlite3* conn, Device& device) const;
  void update(sqlite3* conn, const Device& device,
              std::string_view updated_by = {}) const;
  void erase(sqlite3* conn, std::int64_t id) const;
  void erase_all(sqlite3* conn) const;
  bool exists_triplet(sqlite3* conn, std::string_view name,
                      std::string_view location,
                      std::string_view control_range) const;

  std::vector<std::string> distinct_names(const Db& db) const;
  std::vector<std::string> distinct_locations(const Db& db) const;

 private:
  static std::string where_clause(const DeviceQuery& query);
  static void bind_filters(Stmt& stmt, const DeviceQuery& query, int start_index);
};

}  // namespace devcard
