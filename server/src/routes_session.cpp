#include "routes.h"

#include <cstdint>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/audit.h"
#include "devcard/error.h"
#include "devcard/models.h"
#include "devcard/paths.h"
#include "devcard/service/user_service.h"
#include "devcard/settings.h"
#include "guards.h"
#include "http_json.h"
#include "rate_limit.h"
#include "sessions.h"

namespace devcard::detail {

void register_session_routes(ServerState& state, std::int64_t schema,
                             std::string version) {
  using nlohmann::json;

  state.server.Get("/api/health", [&state, schema, version](
                                     const httplib::Request&,
                                     httplib::Response& response) {
    nlohmann::json body{{"status", "ok"},
                        {"version", version},
                        {"schema_version", schema},
                        {"mode", state.network ? "network" : "local"}};
    // 强制升级握手：只有管理员显式配置了 min_supported_version 才下发，
    // 老服务端/未配置时不出现这个键，客户端按「无要求」处理。
    const std::string min_supported = get_setting(*state.db, "min_supported_version");
    if (!min_supported.empty()) body["min_supported_version"] = min_supported;
    set_json(response, body);
  });
  state.server.Post("/api/session/local",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      if (!is_loopback(request.remote_addr)) {
                        throw AppError("forbidden", "本地会话只能从本机建立");
                      }
                      const json body = parse_body(request.body);
                      const std::string token = body.value("token", "");
                      if (token != state.local_token) {
                        throw AppError("invalid_credentials", "本地会话令牌无效");
                      }
                      const std::string name = state.users->local_user();
                      if (name.empty()) {
                        throw AppError("no_user",
                                       "没有可用账号，请执行 --set-password <用户名>");
                      }
                      const User user = state.users->get(name);
                      set_session_cookie(response, create_session(state, name));
                      set_json(response, json{{"user", user_json(user)}});
                    });
  state.server.Post("/api/session/login",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const json body = parse_body(request.body);
                      const std::string username = body.value("username", "");
                      const std::string password = body.value("password", "");
                      // 先查锁定再验密码：锁定期内即使密码正确也拒，
                      // 否则攻击者只要撞对一次就能把计数清零。
                      const RateLimitDecision limited =
                          rate_limit_check(state, request.remote_addr, username);
                      if (limited.locked) {
                        AuditEntry blocked;
                        blocked.actor = username.empty() ? "anonymous" : username;
                        blocked.actor_ip = request.remote_addr;
                        blocked.action = "login_blocked";
                        blocked.entity = "user";
                        blocked.entity_id = username;
                        blocked.summary = "失败次数过多，429 拒绝（" +
                                          std::to_string(limited.retry_after) +
                                          " 秒后可重试）";
                        audit_event(state, blocked);
                        throw_rate_limited(response, limited);
                      }
                      User user;
                      try {
                        user = state.users->authenticate(username, password);
                      } catch (const AppError& error) {
                        rate_limit_record_failure(state, request.remote_addr,
                                                 username);
                        // 失败也记：撞库在审计里必须能看出来自哪个 IP。
                        AuditEntry entry;
                        entry.actor = username.empty() ? "anonymous" : username;
                        entry.actor_ip = request.remote_addr;
                        entry.action = "login_failed";
                        entry.entity = "user";
                        entry.entity_id = username;
                        entry.summary = error.what();
                        audit_event(state, entry);
                        throw;
                      }
                      // 登录成功清零：同一键的正常使用不该被历史失败拖累。
                      rate_limit_clear(state, request.remote_addr, username);
                      set_session_cookie(response,
                                         create_session(state, user.user_name));
                      AuditEntry entry;
                      entry.actor = user.user_name;
                      entry.actor_ip = request.remote_addr;
                      entry.action = "login";
                      entry.entity = "user";
                      entry.entity_id = user.user_name;
                      entry.summary = "登录成功";
                      audit_event(state, entry);
                      set_json(response, json{{"user", user_json(user)}});
                    });
  state.server.Post("/api/session/logout",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      destroy_session(state, request);
                      clear_session_cookie(response);
                      set_json(response, json{{"ok", true}});
                    });
  state.server.Get("/api/bootstrap", [&state, schema, version](
                                         const httplib::Request& request,
                                         httplib::Response& response) {
    const User user = current_user(state, request);
    set_json(response, json{{"user", user_json(user)},
                            {"version", version},
                            {"schema_version", schema},
                            {"data_dir", path_to_utf8(state.data_dir)},
                            {"company_name", company_name(*state.db)},
                            {"company_name_print", company_print_name(*state.db)},
                            {"company_name_is_default",
                             get_setting(*state.db, "print_company_name").empty()},
                            {"company_name_max_width", kCompanyNameMaxWidth},
                            {"company_logo_url", "/company-logo"},
                            {"company_logo_updated_at",
                             get_setting(*state.db, "company_logo_updated_at")}});
  });

}

}  // namespace devcard::detail
