// 接口契约测试（最小版）：进程内起一次真实服务，把「跨层最容易改坏的四件事」
// 全部锁死。这里断言的不是业务规则，而是前后端共同依赖的契约形状——
// server.cpp 改了响应字段、改了错误码、改了路由名，前端是静默坏掉的，
// 只有这里能拦住：
//
//   1. 认证闸门的顺序契约：Host → CSRF → 401 → must_change_password（403）
//      前端 handleAuthFailure() 按状态码 + error 字段分流，顺序或字段一变就失效。
//   2. 乐观锁 409 响应形状：current / current_version / fields / updated_by。
//      前端冲突弹窗靠这些字段提示「刷新后重填」。
//   3. 前端 JS（app.js + js/*.js）里的每个 /api/ 字面量都能对上服务端路由表：
//      「前端调了不存在的接口」这类回归只能靠比对拦住。
//
// 与 test_web_assets.cpp 同一套起服务方式：进程内 ServerHandle + httplib::Client。
// 会按顺序执行（登录 → 改密 → CRUD → 探测），各段之间有状态依赖，不要并行化。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib.h"  // 必须在 <windows.h> 之前：httplib.h 内部先 include winsock2.h + ws2tcpip.h
                     // 新版 SDK (10.0.26100) 如果先 include <windows.h> 会间接引入 winsock.h，
                     // 导致 ws2tcpip.h 的 SOCKADDR_IN6 multicast 成员被条件编译跳过。

#ifdef _WIN32
#include <windows.h>
#endif

#include "devcard/server.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++checks;                                                                \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond      \
                << "\n";                                                     \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

void set_font_dir_env() {
  // 测试二进制旁边没有随包字体，不指到源码 web/fonts 会直接中止启动。
#ifdef _WIN32
  _putenv_s("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR);
#else
  setenv("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR, 1);
#endif
}

// 服务端路由契约表：method + path + 探测用请求体。
// 探测全部用「必然业务报错」的最小请求：只要服务端回了 AppError 映射的
// 4xx（而不是 httplib 路由未命中的 404「接口不存在」），就证明路由已注册。
// 这里故意不复刻 server.cpp 的注册代码——表漂移 = 契约破坏，测试就该红。
struct RouteProbe {
  const char* method;
  const char* path;  // {id} 会被替换成真实设备 id
  const char* body;  // 非 GET 用的请求体；nullptr 表示不带体
};

const RouteProbe kRoutes[] = {
    // ---- 会话（logout 故意放最后探测，它会销毁会话）----
    {"POST", "/api/session/login", "{"},
    {"POST", "/api/session/local", "{"},
    // ---- 台账 ----
    {"GET", "/api/devices", nullptr},
    {"GET", "/api/devices/options", nullptr},
    {"POST", "/api/devices/import", "{"},
    {"GET", "/api/devices/template", nullptr},
    {"GET", "/api/devices/export", nullptr},
    {"POST", "/api/devices", "{"},
    {"GET", "/api/devices/{id}", nullptr},
    {"PUT", "/api/devices/{id}", "{"},
    {"POST", "/api/devices/{id}/force", "{"},
    {"DELETE", "/api/devices/99999999?version=1", nullptr},
    {"POST", "/api/devices/dedupe", "{"},
    // ---- 用户 ----
    {"GET", "/api/users", nullptr},
    {"POST", "/api/users", "{"},
    {"PUT", "/api/users", "{"},
    {"DELETE", "/api/users", nullptr},
    {"POST", "/api/users/password", "{"},
    // ---- 系统 ----
    {"PUT", "/api/system/company", "{"},
    {"POST", "/api/system/company-logo", "{"},
    {"DELETE", "/api/system/company-logo", nullptr},
    {"GET", "/api/system/backups", nullptr},
    {"POST", "/api/system/backup", "{"},
    {"POST", "/api/system/restore", "{"},
    {"POST", "/api/system/open-data-dir", "{"},
    {"GET", "/api/audit", nullptr},
    // ---- 打印与字体 ----
    {"GET", "/api/print/cards", nullptr},
    {"GET", "/api/fonts/coverage", nullptr},
    {"GET", "/api/bootstrap", nullptr},
    // ---- logout 必须最后（销毁会话）----
    {"POST", "/api/session/logout", "{}"},
};

// app.js 里的动态拼接路径对应的前缀（'/api/devices/' + id、+ id + '/force'）。
const char* kDynamicPrefixes[] = {"/api/devices/"};

}  // namespace

int main() {
  set_font_dir_env();

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path data_dir =
      fs::temp_directory_path() / ("devcard_contract_" + std::to_string(stamp));

  std::unique_ptr<devcard::ServerHandle> server;
  std::string last_error;
  for (int port = 18460; port <= 18480 && !server; ++port) {
    devcard::ServerOptions options;
    options.data_dir = data_dir;
    options.port = port;
    try {
      server = devcard::start_server(options);
    } catch (const std::exception& error) {
      last_error = error.what();
    }
  }
  if (!server) {
    std::cerr << "[失败] 起服务失败：" << last_error << std::endl;
    fs::remove_all(data_dir);
    return 1;
  }

  httplib::Client client("127.0.0.1", server->port());
  client.set_connection_timeout(15, 0);
  client.set_read_timeout(15, 0);
  // Windows 上复用 keep-alive 连接偶尔会撞上半关闭 socket，请求直接失败；
  // 契约测试不关心连接复用，禁用后每次重新建连，避免负载高时误报。
  client.set_keep_alive(false);

  const httplib::Headers kCsrf = {{"X-Requested-With", "DevCard"}};
  std::string cookie;  // 登录后填上，形如 sid=...

  // 首次启动会生成 root / 安业物业 的随机密码，写进数据目录的种子文件。
  // 从文件里取 root 的密码：这是「首次登录必须改密」链路的真实入口。
  std::string root_password;
  {
    std::ifstream in(data_dir / fs::path(u8"初始账号密码.txt"), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const std::size_t sep = line.find("：");
      if (sep != std::string::npos && line.substr(0, sep) == "root") {
        root_password = line.substr(sep + 3);  // 3 字节 = "：" 的 UTF-8 长度
        while (!root_password.empty() &&
               (root_password.back() == '\r' || root_password.back() == '\n')) {
          root_password.pop_back();
        }
      }
    }
  }
  CHECK(!root_password.empty());
  if (root_password.empty()) {
    std::cerr << "[失败] 种子密码文件里没解析到 root 账号" << std::endl;
    server.reset();
    fs::remove_all(data_dir);
    return 1;
  }

  // ===== 1. 认证闸门：顺序与错误形状 =====
  {
    // 未登录：401 + error=unauthorized（handleAuthFailure 靠它弹登录页）
    const auto no_auth = client.Get("/api/devices");
    CHECK(no_auth && no_auth->status == 401);
    if (no_auth) {
      const json body = json::parse(no_auth->body, nullptr, false);
      CHECK(!body.is_discarded() && body.value("error", "") == "unauthorized");
      CHECK(body.contains("message"));
    }
    // 闸门顺序契约：未登录时任何 /api 路径都先撞 401，轮不到路由的 404。
    // 交换「登录检查」和「路由匹配」的顺序不会报错，只会把 401 变 404——
    // 前端会把「接口不存在」当真错误弹出来。
    const auto unknown = client.Get("/api/definitely-not-a-route");
    CHECK(unknown && unknown->status == 401);
    // 非 GET 缺 CSRF 头：403 + error=csrf。必须用非白名单路由探测：
    // /api/session/* 与 /api/health 是「免认证 + 免 CSRF」双白名单，
    // 登录本身必须在无会话时可用，所以 POST login 不带头会直达路由（401）。
    const auto no_csrf = client.Post("/api/devices", "{}", "application/json");
    if (!no_csrf) {
      std::cerr << "[诊断] no_csrf 请求失败 error="
                << static_cast<int>(no_csrf.error()) << std::endl;
    } else if (no_csrf->status != 403) {
      std::cerr << "[诊断] no_csrf status=" << no_csrf->status
                << " body=" << no_csrf->body << std::endl;
    }
    CHECK(no_csrf && no_csrf->status == 403);
    if (no_csrf) {
      const json body = json::parse(no_csrf->body, nullptr, false);
      CHECK(!body.is_discarded() && body.value("error", "") == "csrf");
    }
    // 白名单契约：/api/session/* 免 CSRF——登录 POST 不带头也直达路由，
    // 空凭证回 401 invalid_credentials 而不是 403 csrf。
    const auto login_no_header =
        client.Post("/api/session/login", "{}", "application/json");
    CHECK(login_no_header && login_no_header->status == 401);
    if (login_no_header) {
      const json body = json::parse(login_no_header->body, nullptr, false);
      CHECK(!body.is_discarded() &&
            body.value("error", "") == "invalid_credentials");
    }
    // 免认证白名单：/api/health 无会话也 200
    const auto health = client.Get("/api/health");
    CHECK(health && health->status == 200);
    if (health) {
      const json body = json::parse(health->body, nullptr, false);
      CHECK(!body.is_discarded() && body.value("status", "") == "ok");
      CHECK(body.contains("version") && body.contains("schema_version") &&
            body.contains("mode"));
    }
    // 错误密码：401 + error=invalid_credentials
    const auto bad_login =
        client.Post("/api/session/login", kCsrf,
                    json{{"username", "root"}, {"password", "wrong-password"}}.dump(),
                    "application/json");
    CHECK(bad_login && bad_login->status == 401);
    if (bad_login) {
      const json body = json::parse(bad_login->body, nullptr, false);
      CHECK(!body.is_discarded() &&
            body.value("error", "") == "invalid_credentials");
    }
    // 正确密码：200，但 must_change_password=true
    const auto login =
        client.Post("/api/session/login", kCsrf,
                    json{{"username", "root"}, {"password", root_password}}.dump(),
                    "application/json");
    CHECK(login && login->status == 200);
    if (login) {
      const json body = json::parse(login->body, nullptr, false);
      CHECK(!body.is_discarded());
      CHECK(body["user"].value("must_change_password", false) == true);
      CHECK(body["user"].value("role", "") == "admin");
      const std::string set_cookie = login->get_header_value("Set-Cookie");
      const std::size_t sid = set_cookie.find("sid=");
      CHECK(sid != std::string::npos);
      if (sid != std::string::npos) {
        cookie = set_cookie.substr(sid);
        const std::size_t end = cookie.find(';');
        if (end != std::string::npos) cookie = cookie.substr(0, end);
      }
    }
  }
  if (cookie.empty()) {
    std::cerr << "[失败] 拿不到会话 cookie，后续契约用例全部无法执行" << std::endl;
    server.reset();
    fs::remove_all(data_dir);
    return 1;
  }
  const httplib::Headers auth = {{"Cookie", cookie}, {"X-Requested-With", "DevCard"}};

  // ===== 2. 强制改密闸门 =====
  {
    // must_change_password=true 时，除 /api/bootstrap 与 /api/users/password
    // 外全部 403。前端 openPasswordModal(true) 靠这个状态码触发。
    const auto gated = client.Get("/api/devices", auth);
    CHECK(gated && gated->status == 403);
    if (gated) {
      const json body = json::parse(gated->body, nullptr, false);
      CHECK(!body.is_discarded() &&
            body.value("error", "") == "must_change_password");
    }
    const auto allowed_bootstrap = client.Get("/api/bootstrap", auth);
    CHECK(allowed_bootstrap && allowed_bootstrap->status == 200);
    // 首次改密免旧密码（用户本来就不掌握随机种子密码）
    const auto change = client.Post(
        "/api/users/password", auth,
        json{{"new_password", "Contract#2026-test"}}.dump(), "application/json");
    CHECK(change && change->status == 200);
    // 改密后闸门解除
    const auto after = client.Get("/api/devices", auth);
    CHECK(after && after->status == 200);
  }

  // ===== 3. 乐观锁：创建 → 更新 → 过期提交 409 =====
  std::int64_t device_id = 0;
  {
    const auto created = client.Post(
        "/api/devices", auth,
        json{{"dev_name", "打印机-A1"},
             {"location", "一楼机房"},
             {"control_range", "A-01~A-10"},
             {"phone", "0755-1234567"}}
            .dump(),
        "application/json");
    CHECK(created && created->status == 201);
    if (created && created->status == 201) {
      const json body = json::parse(created->body, nullptr, false);
      device_id = body.value("id", static_cast<std::int64_t>(0));
      CHECK(device_id > 0);
      CHECK(body.value("version", static_cast<std::int64_t>(0)) >= 1);
      // create 不写 updated_by（审计只记 create 动作本身），空串才是真实契约。
      CHECK(body.value("updated_by", "x") == "");
    }

    // 正常更新：带当前 version → 200，版本号 +1
    const std::int64_t current_version = 1;
    const auto updated = client.Put(
        "/api/devices/" + std::to_string(device_id), auth,
        json{{"dev_name", "打印机-B2"},
             {"location", "一楼机房"},
             {"control_range", "A-01~A-10"},
             {"phone", "0755-1234567"},
             {"version", current_version}}
            .dump(),
        "application/json");
    CHECK(updated && updated->status == 200);
    if (updated && updated->status == 200) {
      const json body = json::parse(updated->body, nullptr, false);
      CHECK(body.value("version", static_cast<std::int64_t>(0)) ==
            current_version + 1);
    }

    // 过期提交：还带 version=1 → 409，响应必须带前端冲突弹窗需要的全部字段
    const auto conflict = client.Put(
        "/api/devices/" + std::to_string(device_id), auth,
        json{{"dev_name", "打印机-C3"},
             {"location", "一楼机房"},
             {"control_range", "A-01~A-10"},
             {"phone", "0755-1234567"},
             {"version", current_version}}
            .dump(),
        "application/json");
    CHECK(conflict && conflict->status == 409);
    if (conflict && conflict->status == 409) {
      const json body = json::parse(conflict->body, nullptr, false);
      CHECK(!body.is_discarded() && body.value("error", "") == "conflict");
      CHECK(body.contains("message"));
      CHECK(body.value("current_version", static_cast<std::int64_t>(0)) ==
            current_version + 1);
      CHECK(body.value("updated_by", "") == "root");
      CHECK(body.contains("current") && body["current"].is_object());
      CHECK(body["current"].value("dev_name", "") == "打印机-B2");
      CHECK(body.contains("fields") && body["fields"].is_array());
      bool has_dev_name = false;
      for (const auto& field : body["fields"]) {
        if (field == "dev_name") has_dev_name = true;
      }
      CHECK(has_dev_name);
    }

    // 删除不带 version → 400 bad_version（防止过期页面误删）
    const auto bad_delete =
        client.Delete("/api/devices/" + std::to_string(device_id), auth);
    CHECK(bad_delete && bad_delete->status == 400);
    if (bad_delete) {
      const json body = json::parse(bad_delete->body, nullptr, false);
      CHECK(!body.is_discarded() && body.value("error", "") == "bad_version");
    }
  }

  // ===== 4. 路由表探测：契约表里的每条路由都必须在服务端注册 =====
  {
    // 判据：只要不是 httplib 兜底的 404「接口不存在」就算注册了。
    // 业务 404（设备不存在）会被 set_error_handler 之外的异常分支接管，
    // message 是具体业务文案，不会是「接口不存在」。
    const auto is_route_miss = [](const httplib::Response* r) {
      if (!r || r->status != 404) return false;
      const json body = json::parse(r->body, nullptr, false);
      return body.is_discarded() || body.value("message", "") == "接口不存在";
    };
    for (const RouteProbe& probe : kRoutes) {
      const std::string path = [&] {
        std::string p = probe.path;
        const std::size_t pos = p.find("{id}");
        if (pos != std::string::npos) {
          p.replace(pos, 4, std::to_string(device_id));
        }
        return p;
      }();
      bool ok = false;
      std::string detail;
      if (std::string(probe.method) == "GET") {
        const auto r = client.Get(path, auth);
        ok = !is_route_miss(r ? &*r : nullptr);
        if (r) detail = std::to_string(r->status);
      } else if (std::string(probe.method) == "POST") {
        const auto r = client.Post(path, auth, probe.body ? probe.body : "{}",
                                   "application/json");
        ok = !is_route_miss(r ? &*r : nullptr);
        if (r) detail = std::to_string(r->status);
      } else if (std::string(probe.method) == "PUT") {
        const auto r = client.Put(path, auth, probe.body ? probe.body : "{}",
                                  "application/json");
        ok = !is_route_miss(r ? &*r : nullptr);
        if (r) detail = std::to_string(r->status);
      } else {
        const auto r = client.Delete(path, auth);
        ok = !is_route_miss(r ? &*r : nullptr);
        if (r) detail = std::to_string(r->status);
      }
      ++checks;
      if (!ok) {
        std::cerr << "FAIL 路由未注册或探测失败: " << probe.method << " "
                  << probe.path << " (" << detail << ")" << std::endl;
        ++failures;
      }
    }
  }

  // ===== 5. 前端 JS（app.js + js/*.js）的 /api/ 字面量 ⊆ 服务端路由表 =====
  {
    std::string source;
    auto append_js = [&source](const fs::path& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return false;
      source.append((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
      return true;
    };
    const fs::path web_root(DEVCARD_WEB_SOURCE_DIR);
    CHECK(append_js(web_root / "app.js"));
    std::error_code dir_error;
    for (const auto& entry : fs::directory_iterator(web_root / "js", dir_error)) {
      if (entry.is_regular_file() && entry.path().extension() == ".js") {
        CHECK(append_js(entry.path()));
      }
    }
    CHECK(!source.empty());
    if (!source.empty()) {
      static const std::regex kPattern("/api/[A-Za-z0-9_\\-/]+");
      std::set<std::string> literals;
      for (std::sregex_iterator it = std::sregex_iterator(source.begin(),
                                                          source.end(),
                                                          kPattern);
           it != std::sregex_iterator(); ++it) {
        std::string path = it->str();
        while (!path.empty() && path.back() == '/') path.pop_back();
        if (!path.empty()) literals.insert(path);
      }
      // 正则没工作的话后面全部假绿，先守住数量级。
      CHECK(literals.size() >= 20);

      std::set<std::string> table;
      for (const RouteProbe& probe : kRoutes) table.insert(probe.path);
      for (const std::string& literal : literals) {
        bool covered = table.count(literal) > 0;
        for (const char* prefix : kDynamicPrefixes) {
          if (literal.rfind(prefix, 0) == 0) covered = true;
        }
        ++checks;
        if (!covered) {
          std::cerr << "FAIL 前端调用的接口不在路由契约表里: " << literal
                    << "（改了路由名？把契约表和 server.cpp 一起改）" << std::endl;
          ++failures;
        }
      }
    }
  }

  // ===== 6. 登出契约：logout 探测已执行，会话应已销毁 =====
  {
    const auto after_logout = client.Get("/api/devices", auth);
    CHECK(after_logout && after_logout->status == 401);
  }

  server.reset();
  fs::remove_all(data_dir);

  if (failures == 0) {
    std::cout << "接口契约测试通过：" << checks << " 项断言"
              << "（认证闸门 / 改密闸门 / 乐观锁 409 /"
                 " 路由表探测 / 前端字面量比对）"
              << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
