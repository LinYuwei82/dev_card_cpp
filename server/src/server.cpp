#include "devcard/server.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/backend/sqlite_db.h"
#include "devcard/error.h"
#include "devcard/migrations.h"
#include "devcard/paths.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "devcard/sha256.h"
#include "config.h"
#include "error_status.h"
#include "font_coverage.h"
#include "font_manifest.h"
#include "guards.h"
#include "heartbeat.h"
#include "http_json.h"
#include "instance_lock.h"
#include "logo_store.h"
#include "routes.h"
#include "server_state.h"
#include "sessions.h"
#include "web_assets.h"

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace devcard {

// server 内部实现细节已拆到独立模块；这里只引入 start_server/安全 handler 仍要用的名字。
using detail::ServerState;
using detail::host_allowed;
using detail::is_bypass_path;
using detail::is_loopback;
using detail::load_sessions;
using detail::random_token;
using detail::read_file_bytes;
using detail::session_user;
using detail::set_json;
using detail::status_for;

namespace {

using nlohmann::json;

// 启动耗时打点：每过一个阶段调一次，向 stderr 输出一行
// `[startup] 名称 = X.Xms`（距上次打点的间隔）并重置基准。
// 常驻而非环境变量开关：总共十来行、只在启动路径，验收改前/改后直接对比。
void log_startup(const char* name, std::chrono::steady_clock::time_point& t) {
  const double ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t)
          .count();
  std::cerr << "[startup] " << name << " = " << ms << "ms" << std::endl;
  t = std::chrono::steady_clock::now();
}

unsigned long process_id() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<unsigned long>(getpid());
#endif
}

// 启动第 0 步：内嵌前端与源文件一致性自检。
void verify_embedded_assets(const ServerOptions& options) {
  // 内嵌前端自检：C2026 截断后页面仍能打开，只是内容少了几个字符，光靠肉眼看不出。
  // 这里比对的是「直读 web/ 源文件」的字节数与 SHA256，不是生成出来的 .cpp。
  // --web-dir 开发态不用内嵌资源，自然不查；但仍要求目录里有 index.html，
  // 免得服务起来了才发现首屏 404。
  if (options.web_dir.empty()) {
    const std::string asset_error = embedded_assets_error();
    if (!asset_error.empty()) {
      throw AppError("web_assets", "内嵌前端与源文件不一致：" + asset_error);
    }
  } else if (!std::filesystem::is_regular_file(options.web_dir / "index.html")) {
    throw AppError("bad_config",
                   "--web-dir 下找不到 index.html：" +
                       path_to_utf8(options.web_dir));
  }
}

// 启动第 1 步：把 options 落到 ServerState，并完成目录/会话/锁/库/种子/字体初始化。
void initialize_state(ServerState& state, const ServerOptions& options,
                      std::chrono::steady_clock::time_point& t_startup) {
  state.data_dir = options.data_dir;
  state.web_dir = options.web_dir;
  state.network = options.network;
  state.allowed_hosts = options.allowed_hosts;
  state.rate_limit = options.rate_limit;
  state.bound_host = options.host;
  state.machine_name = local_host_name();

  std::error_code ec;
  std::filesystem::create_directories(options.data_dir, ec);
  if (ec) {
    throw AppError("data_dir", "无法创建数据目录: " + ec.message());
  }
  log_startup("data_dir", t_startup);

  // 恢复上次持久化的会话：进程重启后 WebView2 cookie 还在，server 进程内存空了。
  load_sessions(state);
  log_startup("load_sessions", t_startup);

  // 多实例防护：同一数据目录（规范化后）只能有一个 server 持有写权。
  // 必须在打开数据库之前拿到锁，否则两个进程会先各自打开库再互相覆盖。
  std::string guard_error;
  state.instance_guard = InstanceGuard::acquire(options.data_dir, &guard_error);
  if (!state.instance_guard) {
    throw AppError("instance_in_use", guard_error);
  }
  log_startup("instance_guard", t_startup);

  state.db = open_sqlite_db(options.data_dir / "devicedb.db");
  migrate(*state.db);

  // 第三层防护：库内心跳。锁文件被删除、或两条路径 hardlink 到同一个库时，
  // 只有这一行能判出「还有人在用」。放在库文件身份互斥体之前，
  // 这样验收（删锁文件后再起一个）会走到心跳这条判据上。
  state.instance_id = random_token().substr(0, 16);
  std::string heartbeat_error;
  if (!claim_heartbeat(*state.db, state.instance_id, &heartbeat_error)) {
    throw AppError("instance_in_use", heartbeat_error);
  }

  if (!state.instance_guard->claim_database(options.data_dir / "devicedb.db",
                                            &guard_error)) {
    throw AppError("instance_in_use", guard_error);
  }

  const SeedResult seed = seed_default_users(*state.db, options.data_dir);
  state.seeded = seed.seeded;
  if (seed.seeded) {
    std::cout << "首次启动：已生成初始账号，密码写入 "
              << path_to_utf8(seed.password_file) << "（首次登录必须修改）"
              << std::endl;
    for (const auto& [name, password] : seed.accounts) {
      std::cout << "  用户名: " << name << "  初始密码: " << password << std::endl;
    }
  }

  state.devices = std::make_unique<DeviceService>(*state.db);
  state.users = std::make_unique<UserService>(*state.db);
  state.local_token = random_token();
  state.server.set_payload_max_length(8 * 1024 * 1024);
  log_startup("db_open_migrate_seed", t_startup);
#ifdef _WIN32
  // Windows 的 SO_REUSEADDR 允许第二个进程绑同一端口，两边会随机抢连接；
  // 换成 SO_EXCLUSIVEADDRUSE，让端口冲突在 bind 阶段就明确失败。
  state.server.set_socket_options([](::socket_t sock) {
    int disabled = 0;
    int enabled = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&disabled), sizeof(disabled));
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
  });
#endif

  // 子集字体随包分发，启动时读进内存并逐份校验。
  // 校验值是构建期从 web/fonts/ 读出来的（cmake/font_manifest.cmake），不是「服务器自己报自己的大小」——
  // 后者两边一起错也一致，等于没检查。
  // 缺一份或对不上就中止启动：一个能录台账却永远打不出卡的版本，比启动即失败更糟，
  // 用户会先录一批数据，再发现打不了。
  state.font_dir = default_font_dir();
  for (const auto& spec : kEmbeddedFonts) {
    const auto path = state.font_dir / spec.name;
    std::string bytes = read_file_bytes(path);
    if (bytes.empty()) {
      throw AppError("font_missing",
                     "缺少随包字体 " + path_to_utf8(path) +
                         "：打印与预览必须用同一份子集，缺字体时不做降级，直接中止启动");
    }
    const std::string actual = sha256_hex(bytes);
    if (bytes.size() != spec.bytes || actual != spec.sha256) {
      throw AppError("font_corrupt",
                     "字体校验失败 " + path_to_utf8(path) + "：期望 " +
                         std::to_string(spec.bytes) + " 字节 / SHA256 " + spec.sha256 +
                         "，实得 " + std::to_string(bytes.size()) + " 字节 / SHA256 " + actual +
                         "（多为重新子集后未重建，或文件被截断）");
    }
    state.fonts[spec.name] = std::move(bytes);
  }
  // 「哪些字能印」的唯一判据取自随包字体的 cmap，不做手写清单：
  // 换字体或换字符集时，它自动跟着变。
  const auto regular = state.fonts.find("NotoSansSC-Regular.subset.ttf");
  if (regular != state.fonts.end()) {
    state.coverage = FontCoverage::parse(regular->second);
    if (!state.coverage.error().empty()) {
      std::cerr << "警告：解析子集字体 cmap 失败：" << state.coverage.error() << std::endl;
    }
  }
  log_startup("fonts_verify", t_startup);

}

// 启动第 2 步：挂载全部业务路由。
void register_all_routes(ServerState& state) {
  const auto schema = state.db->schema_version();
  const std::string version = DEVCARD_VERSION;

  register_session_routes(state, schema, version);
  register_device_routes(state);
  register_print_routes(state);
  register_user_routes(state);
  register_system_routes(state);
  register_static_routes(state);

}

// 启动第 3 步：认证闸门与统一响应/异常处理。
void register_security_handlers(ServerState& state) {
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

// 启动第 4 步：绑定端口、启动心跳与监听线程。
void bind_and_serve(ServerState& state, const ServerOptions& options,
                    int& bound_port,
                    std::chrono::steady_clock::time_point& t_startup) {
  if (!state.server.bind_to_port(options.host, options.port)) {
    throw AppError(
        "port_in_use",
        "无法监听 " + options.host + ":" + std::to_string(options.port) +
            "（端口被占用）。请换端口：--port " +
            std::to_string(options.port + 1) +
            "，或查看占用：netstat -ano | findstr :" +
            std::to_string(options.port));
  }
  log_startup("bind", t_startup);
  bound_port = options.port;
  // 锁文件回写监听地址：第二个实例失败时能直接报出占用者在哪个地址上。
  state.instance_guard->set_owner_info(
      "PID " + std::to_string(process_id()) + "，启动于 " + now_text() +
      "，监听 " + options.host + ":" + std::to_string(options.port));
  // 心跳行同步监听地址，失败时第二个实例的报错能指出占用者。
  set_heartbeat_listen(*state.db, state.instance_id,
                       options.host + ":" + std::to_string(options.port));
  state.heartbeat_thread = std::thread([&state]() {
    while (!state.heartbeat_stop) {
      for (int i = 0; i < kHeartbeatIntervalSeconds; ++i) {
        if (state.heartbeat_stop) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      try {
        // 返回 false 说明行已被别的实例接管，自己停止刷新即可。
        if (!refresh_heartbeat(*state.db, state.instance_id)) return;
      } catch (const std::exception& error) {
        // 心跳写失败不终止服务；下一轮重试，连续失败会在下次启动时暴露。
        std::cerr << "警告：心跳写入失败：" << error.what() << std::endl;
      }
    }
  });
  state.thread = std::thread([&state] { state.server.listen_after_bind(); });
  log_startup("listen_thread", t_startup);
}

}  // namespace

struct ServerHandle::Impl : ServerState {};

ServerHandle::ServerHandle() : impl_(std::make_unique<Impl>()) {}
ServerHandle::~ServerHandle() {
  if (!impl_) return;
  impl_->heartbeat_stop = true;
  if (impl_->heartbeat_thread.joinable()) impl_->heartbeat_thread.join();
  if (impl_->server.is_running()) impl_->server.stop();
  if (impl_->thread.joinable()) impl_->thread.join();
  // 正常析构时删掉自己那一行；异常/强杀留下的行由下次启动按 PID/超时接管。
  if (impl_->db && !impl_->instance_id.empty()) {
    try {
      release_heartbeat(*impl_->db, impl_->instance_id);
    } catch (const std::exception& error) {
      std::cerr << "警告：释放库内心跳失败：" << error.what() << std::endl;
    }
  }
}

const std::string& ServerHandle::local_token() const { return impl_->local_token; }

std::unique_ptr<ServerHandle> start_server(const ServerOptions& options) {
  auto t_startup = std::chrono::steady_clock::now();
  verify_embedded_assets(options);
  log_startup("web_assets_check", t_startup);

  auto handle = std::unique_ptr<ServerHandle>(new ServerHandle());
  ServerState& state = *handle->impl_;
  initialize_state(state, options, t_startup);
  register_all_routes(state);
  register_security_handlers(state);
  log_startup("routes_register", t_startup);
  bind_and_serve(state, options, handle->port_, t_startup);
  return handle;
}

}  // namespace devcard
