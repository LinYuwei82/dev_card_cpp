#pragma once

#include <string>
#include <string_view>

#include "devcard/db.h"

namespace devcard {

// app_setting 键值表 DAO：所有 app_setting 的 SQL 只出现在本类实现中。
class SettingDao {
 public:
  std::string get(const Db& db, std::string_view key) const;
  bool has(const Db& db, std::string_view key) const;
  // 写方法要求调用方已处于 Db::write() 事务内。
  void set(sqlite3* conn, std::string_view key, std::string_view value) const;
};

}  // namespace devcard
