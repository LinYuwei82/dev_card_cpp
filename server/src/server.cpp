#include "devcard/server.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include <httplib.h>

#include "devcard/backend/sqlite_db.h"
#include "devcard/error.h"
#include "devcard/migrations.h"
#include "devcard/paths.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "devcard/sha256.h"
#include "config.h"
#include "font_coverage.h"
#include "font_manifest.h"
#include "heartbeat.h"
#include "instance_lock.h"
#include "logo_store.h"
#include "routes.h"
#include "security_handlers.h"
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
using detail::load_sessions;
using detail::random_token;
using detail::read_file_bytes;

namespace {

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
