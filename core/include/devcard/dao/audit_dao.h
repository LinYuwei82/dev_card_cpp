#pragma once

#include <string_view>
#include <vector>

#include "devcard/audit.h"
#include "devcard/db.h"

namespace devcard {

// audit_log 表 DAO：所有审计 SQL 只出现在本类实现中。
class AuditDao {
 public:
  // 写方法要求调用方已处于 Db::write() 事务内。
  void insert(Conn& conn, const AuditEntry& entry,
              std::string_view at) const;
  std::vector<AuditEntry> list(const Db& db, int limit) const;
};

}  // namespace devcard
