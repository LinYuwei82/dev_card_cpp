#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "devcard/db.h"
#include "devcard/models.h"

namespace devcard {

// 用户 DAO：app_user 表的全部 SQL 只出现在本类实现中。
class UserDao {
 public:
  std::vector<User> list(const Db& db) const;
  std::int64_t count(const Db& db) const;
  bool find(const Db& db, std::string_view name, User& out) const;
  bool find(Conn& conn, std::string_view name, User& out) const;

  // 写方法要求调用方已处于 Db::write() 事务内。
  void insert(Conn& conn, const User& user) const;
  bool exists(Conn& conn, std::string_view name) const;
  void update_profile(Conn& conn, std::string_view name,
                      std::string_view display_name, std::string_view role) const;
  void set_password(Conn& conn, std::string_view name,
                    std::string_view password_hash, bool must_change) const;
  void record_failed_login(Conn& conn, std::string_view name, int failed_count,
                           std::string_view locked_until) const;
  void clear_failed_login(Conn& conn, std::string_view name) const;
  void erase(Conn& conn, std::string_view name) const;
  std::int64_t count_admins(Conn& conn) const;
  // 是否还有账号处于「首次登录必须改密」状态。
  bool any_must_change(const Db& db) const;
  // 本地默认账号的候选：管理员优先，其次按用户名排序取第一个。
  std::string first_user_name(const Db& db) const;

 private:
  static void read_row(Stmt& stmt, User& user);
};

}  // namespace devcard
