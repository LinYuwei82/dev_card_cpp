#include "security_handlers.h"

#include <exception>
#include <iostream>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/error.h"
#include "devcard/models.h"
#include "devcard/service/user_service.h"
#include "config.h"
#include "error_status.h"
#include "guards.h"
#include "http_json.h"
#include "sessions.h"

namespace devcard::detail {

void register_security_handlers(ServerState& state) {
  using nlohmann::json;

  state.server.set_pre_routing_handler(
      [&state](const httplib::Request& request, httplib::Response& response) {
        // 校验顺序写死，不要交换：
        //   1 Host 白名单 → 2 来源 IP → 3 路径白名单 → 4 CSRF（非 GET）
        //   → 5 登录 → 6 must_change_password → 7 角色（在各路由的
        //   require_admin 里，晚于本 handler 执行）。
        // 先放行再查 Host，任何一条白名单路径都会顺带绕过 Host 校验；
        // 这类漏洞不报错、也不写日志，只会静默存在。
        if (!host_allowed(state, request.get_header_value("Host"))) {
          set_json(response,
                   json{{"error", "bad_host"},
                        {"message", "Host 头不在允许列表内（防 DNS rebinding）：" +
                                        request.get_header_value("Host")}},
                   421);
          return httplib::Server::HandlerResponse::Handled;
        }
        // 纵深防御：网络模式只接受回环与 RFC1918 来源，公网来源直接 403。
        // 这不能替代防火墙，只是别把库暴露在公网网卡上。
        if (state.network && !is_loopback(request.remote_addr) &&
            !is_private_ipv4(request.remote_addr)) {
          set_json(response,
                   json{{"error", "forbidden_source"},
                        {"message",
                         "网络版只接受本机与内网（10/172.16–31/192.168）来源，"
                         "当前来源 IP：" +
                             request.remote_addr}},
                   403);
          return httplib::Server::HandlerResponse::Handled;
        }
        if (request.path.rfind("/api/", 0) != 0) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        // 路径白名单只读上面那张表，不要在这里另写判断。
        if (is_bypass_path(request.path)) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (request.method != "GET" &&
            request.get_header_value("X-Requested-With") != "DevCard") {
          set_json(response,
                   json{{"error", "csrf"},
                        {"message", "缺少 X-Requested-With: DevCard"}},
                   403);
          return httplib::Server::HandlerResponse::Handled;
        }
        const std::string name = session_user(state, request);
        if (name.empty()) {
          set_json(response,
                   json{{"error", "unauthorized"}, {"message", "未登录或会话已过期"}},
                   401);
          return httplib::Server::HandlerResponse::Handled;
        }
        User user;
        try {
          user = state.users->get(name);
        } catch (const AppError&) {
          set_json(response,
                   json{{"error", "unauthorized"}, {"message", "账号不存在"}}, 401);
          return httplib::Server::HandlerResponse::Handled;
        }
        const bool password_paths =
            request.path == "/api/bootstrap" ||
            request.path == "/api/users/password";
        if (user.must_change_password && !password_paths) {
          set_json(response,
                   json{{"error", "must_change_password"},
                        {"message", "首次登录必须修改密码"}},
                   403);
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });

  state.server.set_post_routing_handler(
      [](const httplib::Request&, httplib::Response& response) {
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("Referrer-Policy", "no-referrer");
        if (response.get_header_value("Content-Type").rfind("text/html", 0) == 0) {
          // connect-src 必须显式放行：更新检查是页面侧 fetch 跑在壳里，
          // 而 default-src 'self' 会把跨域连通性一并禁掉。WebKit 对 CSP 拦截
          // 只报 "Load failed"，看起来像 DNS/TLS 故障，实际请求根本没发出去。
          // 检查地址可由运维配置（含内网 http 清单），所以按 scheme 放行而不写死域名；
          // 脚本仍然只允许 'self'，外部页面注入不进来。
          response.set_header(
              "Content-Security-Policy",
              "default-src 'self'; connect-src 'self' https: http:; "
              "img-src 'self' data:; style-src 'self'");
        }
      });

  state.server.set_error_handler(
      [](const httplib::Request&, httplib::Response& response) {
        if (response.status == 404 && response.body.empty()) {
          set_json(response, json{{"error", "not_found"}, {"message", "接口不存在"}},
                   404);
        }
      });

  state.server.set_exception_handler([](const httplib::Request&,
                                        httplib::Response& response,
                                        std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const AppError& app_error) {
      set_json(response,
               json{{"error", app_error.code()}, {"message", app_error.what()}},
               status_for(app_error));
    } catch (const std::exception& other) {
      // 内部异常文本（sqlite/文件系统细节、绝对路径）只写服务端日志，
      // 客户端统一拿通用文案，避免向登录用户泄露实现信息。
      std::cerr << "警告：未处理异常：" << other.what() << std::endl;
      set_json(response, json{{"error", "internal"}, {"message", "服务器内部错误"}},
               500);
    } catch (...) {
      set_json(response, json{{"error", "internal"}, {"message", "未知错误"}}, 500);
    }
  });
}

}  // namespace devcard::detail
