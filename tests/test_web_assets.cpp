// 内嵌前端自检：确认 web_assets.cpp 里的字符串与 web/ 源文件逐字节一致。
// 这条专抓 MSVC C2026 的静默截断——截断后页面仍能打开，功能却悄悄少了。
//
// 同时校验字体清单：font_manifest.h 里的字节数与 SHA256 是构建期从 web/fonts/
// 读出来的，这里再对着源文件复核一次。它是「先有基准，再谈比对」的那一步——
// 没有这份清单，前端和服务器都只能比字节数，同大小不同内容会被放过去。
//
// 还要起一次真实服务校验 `/` 的 CSP：页面侧 fetch 跨域更新接口，CSP 少了
// connect-src 会被 default-src 兜住，而 WebKit 只报 "Load failed"，看起来像
// DNS/TLS 故障。这条断言就是为了不再靠人记得（2026-11-14 踩过）。
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>

#include "devcard/paths.h"
#include "devcard/server.h"
#include "devcard/sha256.h"
#include "font_manifest.h"
#include "httplib.h"
#include "web_assets.h"

namespace {

std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

int check_fonts() {
  int failures = 0;
  for (const auto& spec : devcard::kEmbeddedFonts) {
    const auto path = std::filesystem::path(DEVCARD_WEB_FONTS_DIR) / spec.name;
    const std::string bytes = read_all(path);
    if (bytes.empty()) {
      std::cerr << "[失败] 字体清单里的文件不存在或为空：" << path << std::endl;
      ++failures;
      continue;
    }
    if (bytes.size() != spec.bytes) {
      std::cerr << "[失败] 字体字节数不符：" << spec.name << " 清单 " << spec.bytes
                << " B，实际 " << bytes.size() << " B" << std::endl;
      ++failures;
      continue;
    }
    if (devcard::sha256_hex(bytes) != spec.sha256) {
      std::cerr << "[失败] 字体 SHA256 不符：" << spec.name << " 清单 "
                << spec.sha256 << "，实际 " << devcard::sha256_hex(bytes) << std::endl;
      ++failures;
    }
  }
  if (failures == 0) {
    std::cout << "字体清单校验通过：" << devcard::kEmbeddedFontCount
              << " 份子集字体的字节数与 SHA256 与源文件一致" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}

// 起一次真实服务，取 `/` 的响应头，检查 CSP 里 connect-src 覆盖了 https: 与 http:。
// 少了它会静默拦掉页面侧的更新检查（fetch 报 Load failed），而且不改任何业务行为，
// 只能靠断言或人记得——断言更可靠。
int check_csp() {
  namespace fs = std::filesystem;
  // 测试二进制旁边没有随包字体，启动会直接中止；指向源码里的字体目录即可。
#ifdef _WIN32
  _putenv_s("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR);
#else
  setenv("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR, 1);
#endif
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path data_dir =
      fs::temp_directory_path() / ("devcard_csp_" + std::to_string(stamp));

  std::unique_ptr<devcard::ServerHandle> server;
  std::string last_error;
  for (int port = 18310; port <= 18330 && !server; ++port) {
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
    std::cerr << "[失败] 起服务失败，无法校验 CSP：" << last_error << std::endl;
    fs::remove_all(data_dir);
    return 1;
  }

  int failures = 0;
  {
    httplib::Client client("127.0.0.1", server->port());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    const auto response = client.Get("/");
    if (!response) {
      std::cerr << "[失败] 取不到 `/`：连接或读取失败" << std::endl;
      failures = 1;
    } else if (response->status != 200) {
      std::cerr << "[失败] `/` 返回 HTTP " << response->status << std::endl;
      failures = 1;
    } else {
      const std::string csp =
          response->get_header_value("Content-Security-Policy");
      const std::size_t head = csp.find("connect-src");
      if (head == std::string::npos) {
        std::cerr << "[失败] `/` 的 CSP 没有 connect-src，跨域 fetch 会被 "
                     "default-src 拦掉（WebKit 报 Load failed）。当前：" << csp
                  << std::endl;
        failures = 1;
      } else {
        std::size_t end = csp.find(';', head);
        if (end == std::string::npos) end = csp.size();
        const std::string connect_src = csp.substr(head, end - head);
        if (connect_src.find("https:") == std::string::npos) {
          std::cerr << "[失败] connect-src 没放行 https:，更新检查无法访问发布接口。当前："
                    << connect_src << std::endl;
          failures = 1;
        }
        if (connect_src.find("http:") == std::string::npos) {
          std::cerr << "[失败] connect-src 没放行 http:，内网 http 清单不可用。当前："
                    << connect_src << std::endl;
          failures = 1;
        }
      }
    }
  }
  server.reset();
  fs::remove_all(data_dir);
  if (failures == 0) {
    std::cout << "CSP 自检通过：`/` 的 connect-src 覆盖 https: 与 http:" << std::endl;
  }
  return failures;
}

std::string seed_password(const std::filesystem::path& data_dir,
                          const std::string& user) {
  std::ifstream in(data_dir / devcard::path_from_utf8("初始账号密码.txt"),
                   std::ios::binary);
  const std::string prefix = user + "：";
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
  }
  return {};
}

// 服务端闸门：检查关闭 → 403 update_disabled；请求的版本不比当前新 → 400 no_update。
// 两条都在下载线程启动前返回，因此不需要 curl，也不会联网。
int check_update_guards() {
  namespace fs = std::filesystem;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path data_dir =
      fs::temp_directory_path() / ("devcard_guard_" + std::to_string(stamp));
  std::unique_ptr<devcard::ServerHandle> server;
  std::string last_error;
  for (int port = 18340; port <= 18360 && !server; ++port) {
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
    std::cerr << "[失败] 起服务失败，无法校验更新闸门：" << last_error << std::endl;
    fs::remove_all(data_dir);
    return 1;
  }

  int failures = 0;
  {
    httplib::Client client("127.0.0.1", server->port());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(10, 0);
    const std::string password = seed_password(data_dir, "root");
    if (password.empty()) {
      std::cerr << "[失败] 找不到 root 初始密码，无法建立会话" << std::endl;
      failures = 1;
    } else {
      const std::string login_body =
          "{\"username\":\"root\",\"password\":\"" + password + "\"}";
      const auto login = client.Post("/api/session/login", login_body,
                                     "application/json");
      std::string cookie;
      if (login && login->status == 200) {
        cookie = login->get_header_value("Set-Cookie");
        cookie = cookie.substr(0, cookie.find(';'));
      }
      if (cookie.empty()) {
        std::cerr << "[失败] 登录失败，取不到会话 cookie" << std::endl;
        failures = 1;
      } else {
        client.set_default_headers({{"Cookie", cookie},
                                    {"X-Requested-With", "DevCard"}});
        const auto change = client.Post(
            "/api/users/password", R"({"new_password":"GuardTest123"})",
            "application/json");
        if (!change || change->status != 200) {
          std::cerr << "[失败] 首次改密失败，无法继续校验下载闸门" << std::endl;
          failures = 1;
        }
        const std::string sha(64, 'a');
        auto payload = [&sha](const std::string& version) {
          return "{\"version\":\"" + version +
                 "\",\"file_name\":\"pkg.bin\",\"url\":"
                 "\"https://github.com/example/pkg.bin\",\"sha256\":\"" + sha +
                 "\",\"size\":10}";
        };

        const auto no_update = client.Post("/api/update/download",
                                           payload("0.0.1"), "application/json");
        if (!no_update || no_update->status != 400 ||
            no_update->body.find("no_update") == std::string::npos) {
          std::cerr << "[失败] 旧版本下载未被拒（期望 400 no_update）："
                    << (no_update ? no_update->status : -1) << " "
                    << (no_update ? no_update->body : "") << std::endl;
          failures = 1;
        }

        {
          std::ofstream out(data_dir / "update.json", std::ios::binary);
          out << R"({"latest":"9.9.9","check_enabled":false})";
        }
        const auto disabled = client.Post("/api/update/download",
                                          payload("9.9.9"), "application/json");
        if (!disabled || disabled->status != 403 ||
            disabled->body.find("update_disabled") == std::string::npos) {
          std::cerr << "[失败] 检查关闭时仍可下载（期望 403 update_disabled）："
                    << (disabled ? disabled->status : -1) << " "
                    << (disabled ? disabled->body : "") << std::endl;
          failures = 1;
        }

        if (fs::exists(data_dir / "updates")) {
          std::cerr << "[失败] 被拒绝的下载请求仍然创建了 updates 目录" << std::endl;
          failures = 1;
        }
      }
    }
  }
  server.reset();
  fs::remove_all(data_dir);
  if (failures == 0) {
    std::cout << "更新闸门自检通过：更新检查关闭返回 403、无新版本返回 400"
              << std::endl;
  }
  return failures;
}

// 状态码表：路由会抛的每个错误码都必须有对应 HTTP 状态。
// 缺一条的后果是 4xx 被渲染成 500（「打开数据目录」失败那次就是这么藏的），
// 所以这里不只查表，还把 server.cpp 里真实的 AppError 字面量扫一遍——
// 新增 throw 忘了补表时，这条用例直接红，不用等线上。
int check_status_map() {
  int failures = 0;
  const auto& codes = devcard::route_error_codes();
  for (const std::string& code : codes) {
    if (!devcard::http_status_known(code)) {
      std::cerr << "[失败] 路由错误码 " << code << " 在 status_for 表里没有对应项" << std::endl;
      ++failures;
    }
  }

  // 反向：表里不许有孤儿项（登记了但没有任何路由会抛），否则表会越滚越大、
  // 掩盖「这个码其实已经没人抛了」的事实。
  const std::set<std::string> listed(codes.begin(), codes.end());
  for (const std::string& code : devcard::mapped_error_codes()) {
    if (!listed.count(code)) {
      std::cerr << "[失败] status_for 表里的 \"" << code
                << "\" 是孤儿项：没有路由会抛它，请从表里删掉或补进清单" << std::endl;
      ++failures;
    }
  }

  // 未映射类型必须能被认出来（不能和「已映射」混在一起）。
  if (devcard::http_status_known("__unmapped_probe__")) {
    std::cerr << "[失败] 未映射的错误码被当成了已知类型" << std::endl;
    ++failures;
  }
  if (devcard::http_status_for("__unmapped_probe__") != 500) {
    std::cerr << "[失败] 未映射的错误码没有回落到 500" << std::endl;
    ++failures;
  }

  // 逐个钉住关键码，避免有人改表时静默漂移。
  struct Expected {
    const char* code;
    int status;
  };
  const Expected expected[] = {
      // 6f425fd 七项 + 下载路由
      {"endpoint_not_allowed", 400}, {"no_update", 400},
      {"update_disabled", 403},      {"update_busy", 409},
      {"manifest_invalid", 502},     {"download_failed", 502},
      {"checksum_mismatch", 502},
      // 同类洞：未登录不是服务端错误
      {"unauthorized", 401},
  };
  for (const Expected& item : expected) {
    const int actual = devcard::http_status_for(item.code);
    if (actual != item.status) {
      std::cerr << "[失败] " << item.code << " 期望 " << item.status << "，实得 "
                << actual << std::endl;
      ++failures;
    }
  }

  // 源码扫描：server.cpp 里直接 throw 的码，要么在清单里，要么是启动期错误。
  std::ifstream in(DEVCARD_SERVER_SOURCE, std::ios::binary);
  if (!in) {
    std::cerr << "[失败] 读不到 " << DEVCARD_SERVER_SOURCE << std::endl;
    return failures + 1;
  }
  const std::string source{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
  const std::string needle = "AppError(\"";
  // 启动阶段（还没开始服务）抛的码不经过路由，不需要 HTTP 状态。
  const std::set<std::string> startup_only = {"web_assets", "data_dir",
                                              "font_missing", "font_corrupt",
                                              "port_in_use", "instance_in_use"};
  std::size_t pos = 0;
  while ((pos = source.find(needle, pos)) != std::string::npos) {
    pos += needle.size();
    const std::size_t end = source.find('"', pos);
    if (end == std::string::npos) break;
    const std::string code = source.substr(pos, end - pos);
    pos = end;
    if (startup_only.count(code)) continue;
    if (!listed.count(code)) {
      std::cerr << "[失败] server.cpp 抛出了未登记的错误码 \"" << code
                << "\"：请同时加进 route_error_codes() 与 status_for 的表"
                << std::endl;
      ++failures;
    }
  }

  if (failures == 0) {
    std::cout << "状态码自检通过：路由错误码 " << codes.size()
              << " 项全部有对应 HTTP 状态，表内无孤儿项，未映射码回落 500"
              << std::endl;
  }
  return failures;
}

}  // namespace

int main() {
  int code = 0;
  const std::string error = devcard::embedded_assets_error();
  if (!error.empty()) {
    std::cerr << "[失败] 内嵌前端与源文件不一致：" << error << std::endl;
    code = 1;
  } else {
    std::cout << "内嵌前端自检通过：index.html / styles.css / app.js / logo.jpg 与源文件一致"
              << "（index.html " << devcard::embedded_index_html_size() << " B, styles.css "
              << devcard::embedded_styles_css_size() << " B, app.js "
              << devcard::embedded_app_js_size() << " B）" << std::endl;
  }
  if (check_fonts() != 0) {
    code = 1;
  }
  if (check_csp() != 0) {
    code = 1;
  }
  if (check_update_guards() != 0) {
    code = 1;
  }
  if (check_status_map() != 0) {
    code = 1;
  }
  return code;
}
