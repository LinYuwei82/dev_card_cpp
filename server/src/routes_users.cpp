#include "routes.h"

#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/error.h"
#include "devcard/models.h"
#include "devcard/service/user_service.h"
#include "http_json.h"
#include "rate_limit.h"
#include "sessions.h"

namespace devcard::detail {

void register_user_routes(ServerState& state) {
  using nlohmann::json;

  state.server.Get("/api/users", [&state](const httplib::Request& request,
                                          httplib::Response& response) {
    require_admin(state, request);
    json items = json::array();
    for (const User& user : state.users->list()) items.push_back(user_json(user));
    set_json(response, json{{"items", items}});
  });
  state.server.Post("/api/users", [&state](const httplib::Request& request,
                                           httplib::Response& response) {
    require_admin(state, request);
    const json body = parse_body(request.body);
    const User user = state.users->create(
        body.value("user_name", ""), body.value("password", ""),
        body.value("display_name", ""), body.value("role", "user"));
    set_json(response, json{{"user", user_json(user)}}, 201);
  });
  state.server.Put("/api/users", [&state](const httplib::Request& request,
                                          httplib::Response& response) {
    require_admin(state, request);
    const json body = parse_body(request.body);
    const User user = state.users->update_profile(
        body.value("user_name", ""), body.value("display_name", ""),
        body.value("role", "user"));
    set_json(response, json{{"user", user_json(user)}});
  });
  state.server.Delete("/api/users", [&state](const httplib::Request& request,
                                             httplib::Response& response) {
    const User actor = require_admin(state, request);
    const std::string name = request.get_param_value("name");
    state.users->remove(name, actor.user_name);
    drop_sessions_for(state, name);
    set_json(response, json{{"ok", true}});
  });
  state.server.Post("/api/users/password",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = current_user(state, request);
                      // 改密同样双键计数：猜旧密码与撞登录密码是同一类攻击。
                      const RateLimitDecision limited =
                          rate_limit_check(state, request.remote_addr,
                                           actor.user_name);
                      if (limited.locked) throw_rate_limited(response, limited);
                      std::string target;
                      try {
                        const json body = parse_body(request.body);
                        target = body.value("user_name", "");
                        if (target.empty()) target = actor.user_name;
                        const std::string new_password = body.value("new_password", "");
                        if (new_password.empty()) {
                          throw AppError("bad_json", "new_password 不能为空");
                        }
                        if (target == actor.user_name) {
                          // 是否免旧密码只由服务端状态决定：本次是首次登录或管理员
                          // 重置后的强制改密时，用户本来就不掌握旧密码。
                          if (actor.must_change_password) {
                            state.users->complete_forced_change(actor.user_name,
                                                                new_password);
                          } else {
                            const std::string old_password =
                                body.value("old_password", "");
                            if (old_password.empty()) {
                              throw AppError("old_password_required", "请输入原密码");
                            }
                            state.users->change_password(actor.user_name, old_password,
                                                         new_password);
                          }
                        } else {
                          if (actor.role != "admin") {
                            throw AppError("forbidden", "需要管理员权限");
                          }
                          state.users->reset_password(target, new_password);
                        }
                        if (!state.users->any_must_change()) {
                          remove_seed_password_file(state.data_dir);
                        }
                        // 改密 / 重置后旧会话一律作废；改自己的密码时保留当前这条，
                        // 免得刚改完就把自己踢回登录页。
                        const std::string keep =
                            target == actor.user_name
                                ? cookie_value(request, "sid")
                                : std::string{};
                        drop_sessions_for(state, target, keep);
                        set_json(response, json{{"ok", true}, {"user_name", target}});
                      } catch (const AppError&) {
                        // 只有冲着操作者自己来的失败才计数：管理员重置别人的密码失败
                        // （比如用户名打错）不该把管理员自己锁在门外。
                        if (target.empty() || target == actor.user_name) {
                          rate_limit_record_failure(state, request.remote_addr,
                                                    actor.user_name);
                        }
                        throw;
                      }
                      rate_limit_clear(state, request.remote_addr, actor.user_name);
                    });

}

}  // namespace devcard::detail
