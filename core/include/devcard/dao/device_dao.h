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
  // 乐观锁：仅当 device.version 与库里一致时才更新，成功把 version 加一。
  // 返回 false 表示版本不匹配（或被删除），调用方负责区分 409/404。
  bool update(sqlite3* conn, const Device& device,
              std::string_view updated_by = {}) const;
  // 删除同样校验版本，避免「别人刚改过就被旧页面删掉」。
  bool erase(sqlite3* conn, std::int64_t id, std::int64_t version) const;
  void erase_all(sqlite3* conn) const;
  // 合并三字段（名称 + 位置 + 控制范围）完全相同的记录：每组只留 id 最小的一行，
  // 保留行电话为空时用组内最早的非空电话补齐（补齐会 version+1，旧页面不会静默覆盖）。
  // 返回被删除的重复行数。
  std::int64_t dedupe(sqlite3* conn, std::string_view updated_by = {}) const;
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
