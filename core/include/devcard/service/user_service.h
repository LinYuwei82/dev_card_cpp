#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "devcard/dao/user_dao.h"
#include "devcard/db.h"
#include "devcard/models.h"

namespace devcard {

struct SeedResult {
  bool seeded = false;
  std::filesystem::path password_file;  // 首次生成的一次性口令文件
  std::vector<std::pair<std::string, std::string>> accounts;  // 用户名 + 一次性口令
};

// 口令策略：8–128 个字符。
void validate_password(std::string_view password);
std::string validate_user_name(std::string_view name);

// 用户与认证：Argon2id 校验、5 次失败锁 15 分钟、角色与强制改密。
class UserService {
 public:
  explicit UserService(Db& db) : db_(db) {}

  bool empty() const;
  std::int64_t count() const;
  std::vector<User> list() const;
  User get(std::string_view name) const;

  // 登录：失败会累计，第 5 次失败锁定 15 分钟；错误信息不区分账号是否存在。
  User authenticate(std::string_view name, std::string_view password);

  User create(std::string name, std::string password, std::string display_name,
              std::string role);
  User update_profile(std::string_view name, std::string display_name,
                      std::string role);
  void remove(std::string_view name, std::string_view actor);

  // 自助改密：校验旧口令，成功后清除强制改密标记。
  void change_password(std::string_view name, std::string_view old_password,
                       std::string_view new_password);
  // 管理员/命令行重置：不校验旧口令，强制下次登录改密。
  void reset_password(std::string_view name, std::string_view new_password);

  bool any_must_change() const;
  // 本机壳自动登录使用的账号：app_setting.local_user → 第一个管理员 → 第一个用户。
  std::string local_user() const;

 private:
  Db& db_;
  UserDao dao_;
};

// 全新数据库首次启动时生成两个强制改密的账号，口令随机并写入
// <data_dir>/初始账号口令.txt（权限 0600）。已有用户时不做任何事。
SeedResult seed_default_users(Db& db, const std::filesystem::path& data_dir);

// 所有账号都已完成首次改密后，删除一次性口令文件。
void remove_seed_password_file(const std::filesystem::path& data_dir);

}  // namespace devcard
