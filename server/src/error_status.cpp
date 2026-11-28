#include "error_status.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "devcard/error.h"
#include "devcard/server.h"

namespace devcard {
namespace {

using StatusTable = std::map<std::string, int, std::less<>>;

const StatusTable& error_status_table() {
  // 单张表：错误码 → HTTP。新增码只加一行，不再套 if 链。
  // 凡是路由会抛的类型都必须在这里，缺一条就会把 4xx 渲染成 500。
  static const StatusTable kStatus = {
      {"duplicate", 409},
      {"last_admin", 409},
      {"self_delete", 409},
      {"no_user", 409},
      {"not_found", 404},
      {"legacy_db", 409},
      {"invalid_credentials", 401},
      {"unauthorized", 401},
      // 登录/改密失败限流：单独一项，不混进 401/403——客户端要能按 429
      // 读 Retry-After 决定等待，而不是把「密码错了」和「先别试了」混为一谈。
      {"too_many_requests", 429},
      {"locked", 423},
      {"forbidden", 403},
      {"must_change_password", 403},
      {"bad_request", 400},
      {"bad_version", 400},
      // 更新包落点必须是绝对路径：用户选了相对路径属于请求错误。
      {"invalid_path", 400},
      {"invalid_device", 400},
      {"invalid_user", 400},
      {"weak_password", 400},
      {"bad_field", 400},
      {"bad_json", 400},
      {"bad_header", 400},
      {"bad_xlsx", 400},
      {"bad_backup", 400},
      // 备份文件本身打不开：是用户选错了文件，不是服务端故障。
      {"backup_invalid", 400},
      {"old_password_required", 400},
      {"bad_logo", 400},
      {"logo_too_large", 400},
      {"bad_company_name", 400},
      {"logo_write", 400},
      // 路由可达，但属于服务端 / 磁盘故障：显式写出来，别靠「没登记就 500」兜着。
      {"backup_failed", 500},
      {"io_error", 500},
      {"db_error", 500},
      // core/src 里会抛、且能经路由走到的码：不登记就会落到 500 兜底，
      // 前端拿到的 error 字段会从具体码变成空，排障时看不出是哪一层。
      {"db_exec", 500},
      {"db_unexpected_row", 500},
      {"password_hash", 500},
      {"xlsx_write_failed", 500},
  };
  return kStatus;
}

}  // namespace

int http_status_for(std::string_view code) {
  const StatusTable& table = error_status_table();
  const auto found = table.find(code);
  return found == table.end() ? 500 : found->second;
}

bool http_status_known(std::string_view code) {
  const StatusTable& table = error_status_table();
  return table.find(code) != table.end();
}

const std::vector<std::string>& route_error_codes() {
  // 与各路由里 throw AppError(...) 的 code 对应。新增路由错误时，这里和
  // error_status_table 要一起补，否则 tests 里的「清单覆盖」用例会红。
  static const std::vector<std::string> kCodes = {
      "duplicate",        "last_admin",       "self_delete",
      "no_user",          "not_found",        "legacy_db",
      "invalid_credentials", "unauthorized",  "locked",
      "too_many_requests",
      "forbidden",        "must_change_password",
      "invalid_device",   "invalid_user",     "weak_password",
      "bad_field",        "bad_json",         "bad_header",
      "bad_xlsx",         "bad_backup",
      "backup_invalid",   "old_password_required",
      "bad_logo",         "logo_too_large",   "bad_company_name",
      "logo_write",
      "backup_failed",    "io_error",         "db_error",
      "db_exec",          "db_unexpected_row", "password_hash",
      "xlsx_write_failed", "bad_version",
      "invalid_path",
      "bad_request",
  };
  return kCodes;
}

const std::vector<std::string>& mapped_error_codes() {
  static const std::vector<std::string> kCodes = [] {
    std::vector<std::string> codes;
    for (const auto& item : error_status_table()) codes.push_back(item.first);
    return codes;
  }();
  return kCodes;
}

namespace detail {

int status_for(const AppError& error) { return http_status_for(error.code()); }

}  // namespace detail

}  // namespace devcard
