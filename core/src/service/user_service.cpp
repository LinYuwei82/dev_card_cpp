#include "devcard/service/user_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "devcard/error.h"
#include "devcard/password.h"
#include "devcard/paths.h"
#include "devcard/settings.h"

namespace devcard {
namespace {

constexpr int kMaxFailures = 5;
constexpr int kLockMinutes = 15;

std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::size_t utf8_length(const std::string& value) {
  std::size_t count = 0;
  for (unsigned char c : value) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

std::string now_plus_minutes(int minutes) {
  using namespace std::chrono;
  const auto target = system_clock::now() + minutes * 60s;
  const std::time_t t = system_clock::to_time_t(target);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
  return buffer;
}

void require_role(std::string_view role) {
  if (role != "admin" && role != "user") {
    throw AppError("invalid_user", "角色只能是 admin 或 user");
  }
}

}  // namespace

void validate_password(std::string_view password) {
  const std::size_t length = utf8_length(std::string(password));
  if (length < 8 || length > 128) {
    throw AppError("weak_password", "密码长度需为 8–128 个字符");
  }
}

std::string validate_user_name(std::string_view name) {
  std::string value = trim(std::string(name));
  if (value.empty()) throw AppError("invalid_user", "用户名不能为空");
  if (utf8_length(value) > 20) {
    throw AppError("invalid_user", "用户名不能超过 20 个字符");
  }
  for (unsigned char c : value) {
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') {
      throw AppError("invalid_user", "用户名不能包含控制字符或路径分隔符");
    }
  }
  return value;
}

bool UserService::empty() const { return dao_.count(db_) == 0; }

std::int64_t UserService::count() const { return dao_.count(db_); }

std::vector<User> UserService::list() const { return dao_.list(db_); }

User UserService::get(std::string_view name) const {
  User user;
  if (!dao_.find(db_, name, user)) throw AppError("not_found", "用户不存在");
  return user;
}

User UserService::authenticate(std::string_view name, std::string_view password) {
  User user;
  if (!dao_.find(db_, name, user)) {
    throw AppError("invalid_credentials", "用户名或密码错误");
  }
  if (!user.locked_until.empty() && user.locked_until > now_text()) {
    throw AppError("locked", "登录失败次数过多，账号已锁定，请稍后再试");
  }
  if (!verify_password(user.password_hash, password)) {
    int failed = user.failed_count + 1;
    std::string locked_until;
    if (failed >= kMaxFailures) {
      locked_until = now_plus_minutes(kLockMinutes);
      failed = 0;
    }
    db_.write([&](sqlite3* conn) {
      dao_.record_failed_login(conn, user.user_name, failed, locked_until);
    });
    throw AppError("invalid_credentials", "用户名或密码错误");
  }
  if (user.failed_count != 0 || !user.locked_until.empty()) {
    db_.write([&](sqlite3* conn) { dao_.clear_failed_login(conn, user.user_name); });
    user.failed_count = 0;
    user.locked_until.clear();
  }
  return user;
}

User UserService::create(std::string name, std::string password,
                         std::string display_name, std::string role) {
  User user;
  user.user_name = validate_user_name(name);
  validate_password(password);
  require_role(role);
  user.display_name = trim(display_name).empty() ? user.user_name
                                                 : trim(display_name);
  if (utf8_length(user.display_name) > 20) {
    throw AppError("invalid_user", "显示名不能超过 20 个字符");
  }
  user.role = role;
  user.password_hash = hash_password(password);
  user.must_change_password = true;

  db_.write([&](sqlite3* conn) {
    if (dao_.exists(conn, user.user_name)) {
      throw AppError("duplicate", "用户名已存在");
    }
    dao_.insert(conn, user);
  });
  return get(user.user_name);
}

User UserService::update_profile(std::string_view name, std::string display_name,
                                 std::string role) {
  require_role(role);
  const std::string display = trim(display_name);
  if (utf8_length(display) > 20) {
    throw AppError("invalid_user", "显示名不能超过 20 个字符");
  }
  db_.write([&](sqlite3* conn) {
    User current;
    if (!dao_.find(conn, name, current)) {
      throw AppError("not_found", "用户不存在");
    }
    if (current.role == "admin" && role != "admin" &&
        dao_.count_admins(conn) <= 1) {
      throw AppError("last_admin", "至少保留一个管理员账号");
    }
    dao_.update_profile(conn, name, display.empty() ? current.user_name : display,
                        role);
  });
  return get(name);
}

void UserService::remove(std::string_view name, std::string_view actor) {
  if (name == actor) {
    throw AppError("self_delete", "不能删除当前登录账号");
  }
  db_.write([&](sqlite3* conn) {
    User current;
    if (!dao_.find(conn, name, current)) {
      throw AppError("not_found", "用户不存在");
    }
    if (current.role == "admin" && dao_.count_admins(conn) <= 1) {
      throw AppError("last_admin", "至少保留一个管理员账号");
    }
    dao_.erase(conn, name);
  });
}

void UserService::change_password(std::string_view name,
                                  std::string_view old_password,
                                  std::string_view new_password) {
  User user = get(name);
  if (!verify_password(user.password_hash, old_password)) {
    throw AppError("invalid_credentials", "原密码错误");
  }
  validate_password(new_password);
  const std::string hash = hash_password(new_password);
  db_.write([&](sqlite3* conn) { dao_.set_password(conn, name, hash, false); });
}

void UserService::complete_forced_change(std::string_view name,
                                         std::string_view new_password) {
  const User user = get(name);
  if (!user.must_change_password) {
    // 非强制改密场景必须走 change_password 校验旧密码。
    throw AppError("old_password_required", "请输入原密码");
  }
  validate_password(new_password);
  const std::string hash = hash_password(new_password);
  db_.write([&](sqlite3* conn) { dao_.set_password(conn, name, hash, false); });
}

void UserService::reset_password(std::string_view name,
                                 std::string_view new_password) {
  get(name);
  validate_password(new_password);
  const std::string hash = hash_password(new_password);
  db_.write([&](sqlite3* conn) { dao_.set_password(conn, name, hash, true); });
}

bool UserService::any_must_change() const { return dao_.any_must_change(db_); }

std::string UserService::local_user() const {
  const std::string configured = get_setting(db_, "local_user");
  if (!configured.empty()) {
    User user;
    if (dao_.find(db_, configured, user)) return configured;
  }
  return dao_.first_user_name(db_);
}

SeedResult seed_default_users(Db& db, const std::filesystem::path& data_dir) {
  SeedResult result;
  UserDao dao;
  if (dao.count(db) > 0) return result;

  struct Seed {
    const char* name;
    const char* role;
  };
  static const Seed seeds[] = {{"root", "admin"}, {"安业物业", "user"}};

  std::vector<std::pair<std::string, std::string>> accounts;
  std::vector<User> users;
  for (const Seed& seed : seeds) {
    User user;
    user.user_name = seed.name;
    user.display_name = seed.name;
    user.role = seed.role;
    user.must_change_password = true;
    const std::string password = random_password();
    user.password_hash = hash_password(password);
    accounts.emplace_back(user.user_name, password);
    users.push_back(std::move(user));
  }

  db.write([&](sqlite3* conn) {
    for (const User& user : users) dao.insert(conn, user);
    set_setting(conn, "local_user", "root");
  });

  // 文件名是 UTF-8 字面量，必须走 path_from_utf8：Windows 上 path 的窄字符构造
  // 按 ACP（GBK）解释，直接拼会落盘成乱码目录项。
  const std::filesystem::path file = data_dir / path_from_utf8("初始账号密码.txt");
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out) {
      out << "DevCard 初始账号（首次登录必须修改密码）\n";
      for (const auto& [name, password] : accounts) {
        out << name << "：" << password << "\n";
      }
      out << "\n修改完成后本文件会自动删除。\n";
    }
  }
#ifndef _WIN32
  ::chmod(file.c_str(), 0600);
#endif

  result.seeded = true;
  result.password_file = file;
  result.accounts = std::move(accounts);
  return result;
}

void remove_seed_password_file(const std::filesystem::path& data_dir) {
  std::error_code ec;
  std::filesystem::remove(data_dir / path_from_utf8("初始账号密码.txt"), ec);
}

}  // namespace devcard
