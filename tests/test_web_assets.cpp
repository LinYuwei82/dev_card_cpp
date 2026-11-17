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
#include <thread>

#include "config.h"
#include "devcard/error.h"
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

// 字体闸门的负向自检：正向清单只能证明「当前文件没问题」，证明不了坏字体真的会被拒绝。
// 两种坏法各起一次服务：目录里没有字体 → font_missing；文件被截断 → font_corrupt。
// 判据是 start_server 抛 AppError 且错误码与文案都对；若服务起来了，说明闸门形同虚设。
int check_font_gate() {
  namespace fs = std::filesystem;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root =
      fs::temp_directory_path() / ("devcard_fontgate_" + std::to_string(stamp));
  const fs::path empty_dir = root / "no-fonts";
  const fs::path corrupt_dir = root / "corrupt";
  std::error_code ec;
  fs::create_directories(empty_dir, ec);
  fs::create_directories(corrupt_dir, ec);

  // 先复制一份完整字体，再把 Bold 截掉 100 字节：文件名不动，只让字节数对不上。
  const std::string bold_name = "NotoSansSC-Bold.subset.ttf";
  for (const auto& spec : devcard::kEmbeddedFonts) {
    ec.clear();
    fs::copy_file(fs::path(DEVCARD_WEB_FONTS_DIR) / spec.name,
                  corrupt_dir / spec.name, fs::copy_options::overwrite_existing,
                  ec);
    if (ec) {
      std::cerr << "[失败] 准备字体闸门用例时复制失败：" << spec.name << "："
                << ec.message() << std::endl;
      fs::remove_all(root);
      return 1;
    }
  }
  const fs::path truncated = corrupt_dir / bold_name;
  ec.clear();
  fs::resize_file(truncated, fs::file_size(truncated) - 100, ec);
  if (ec) {
    std::cerr << "[失败] 截断字体失败：" << ec.message() << std::endl;
    fs::remove_all(root);
    return 1;
  }

  std::string old_font_dir;
  const bool had_old = std::getenv("DEVCARD_FONT_DIR") != nullptr;
  if (had_old) old_font_dir = std::getenv("DEVCARD_FONT_DIR");
  const auto set_font_dir = [](const std::string& value) {
#ifdef _WIN32
    _putenv_s("DEVCARD_FONT_DIR", value.c_str());
#else
    setenv("DEVCARD_FONT_DIR", value.c_str(), 1);
#endif
  };

  int failures = 0;
  const auto expect_gate = [&](const fs::path& font_dir, const fs::path& data_dir,
                               const char* want_code, const char* want_text,
                               const char* label) {
    set_font_dir(font_dir.string());
    devcard::ServerOptions options;
    options.data_dir = data_dir;
    options.port = 18390;
    std::unique_ptr<devcard::ServerHandle> server;
    std::string error_code;
    std::string error_text;
    try {
      server = devcard::start_server(options);
    } catch (const devcard::AppError& error) {
      error_code = error.code();
      error_text = error.what();
    } catch (const std::exception& error) {
      error_text = error.what();
    }
    if (server) {
      server.reset();
      std::cerr << "[失败] " << label << "：坏字体目录竟然启动了服务，闸门未拦住"
                << std::endl;
      ++failures;
      return;
    }
    if (error_code != want_code) {
      std::cerr << "[失败] " << label << "：期望错误码 " << want_code << "，实得 "
                << (error_code.empty() ? "（非 AppError）" : error_code) << "："
                << error_text << std::endl;
      ++failures;
      return;
    }
    if (error_text.find(want_text) == std::string::npos) {
      std::cerr << "[失败] " << label << "：错误信息缺少「" << want_text << "」："
                << error_text << std::endl;
      ++failures;
    }
  };

  expect_gate(empty_dir, root / "data-missing", "font_missing", "缺少随包字体",
              "缺字体");
  expect_gate(corrupt_dir, root / "data-corrupt", "font_corrupt", "字体校验失败",
              "字体被截断");

  // 还原现场：后面的用例还要用正常字体目录起服务。
  if (had_old) {
    set_font_dir(old_font_dir);
  } else {
#ifdef _WIN32
    _putenv_s("DEVCARD_FONT_DIR", "");
#else
    unsetenv("DEVCARD_FONT_DIR");
#endif
  }
  fs::remove_all(root);
  if (failures == 0) {
    std::cout << "字体闸门负向自检通过：缺字体 → font_missing，截断 → font_corrupt，"
                 "均中止启动"
              << std::endl;
  }
  return failures;
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

        // 分平台最新版本：macOS 与 Windows 发版节奏不同步时，
        // 各自只应看到本平台的 latest，不能用全局 latest 冒充。
        {
          std::ofstream out(data_dir / "update.json", std::ios::binary);
          out << R"({"latest":"1.2.2","latest_by_platform":)"
                 R"({"macos":"9.9.9","windows":"1.2.2"},)"
                 R"("check_enabled":false})";
        }
#ifdef _WIN32
        const std::string expect_latest = "1.2.2";
        const std::string expect_available = "false";
#else
        const std::string expect_latest = "9.9.9";
        const std::string expect_available = "true";
#endif
        const auto version_info = client.Get("/api/version");
        const std::string version_body =
            version_info ? version_info->body : std::string{};
        if (!version_info || version_info->status != 200 ||
            version_body.find("\"latest\":\"" + expect_latest + "\"") ==
                std::string::npos ||
            version_body.find(std::string("\"update_available\":") +
                              expect_available) == std::string::npos) {
          std::cerr << "[失败] 分平台 latest 未生效（期望 "
                    << expect_latest << " / update_available="
                    << expect_available << "）："
                    << (version_info ? version_info->status : -1) << " "
                    << version_body << std::endl;
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

// 登录/改密失败限流：真起服务、真走 HTTP。
// 回环地址按要求整体不受限，所以限流那几段必须从非回环来源打——把服务绑在
// 本机私有 IP 上（网络模式），否则测出来的永远是不受限的那条路径。
int check_rate_limit() {
  namespace fs = std::filesystem;
#ifdef _WIN32
  _putenv_s("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR);
#else
  setenv("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR, 1);
#endif
  const std::vector<std::string> addresses = devcard::private_ipv4_addresses();
  if (addresses.empty()) {
    std::cerr << "[失败] 没有私有 IPv4，无法从非回环来源验证限流" << std::endl;
    return 1;
  }
  const std::string address = addresses.front();

  auto start = [](const std::string& prefix, const std::string& host, int base,
                  bool enabled, fs::path* data_dir) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    *data_dir = fs::temp_directory_path() / (prefix + std::to_string(stamp));
    std::unique_ptr<devcard::ServerHandle> server;
    for (int port = base; port <= base + 20 && !server; ++port) {
      devcard::ServerOptions options;
      options.data_dir = *data_dir;
      options.host = host;
      options.port = port;
      options.network = host != "127.0.0.1";
      options.rate_limit.enabled = enabled;
      options.rate_limit.max_failures = 5;
      options.rate_limit.window_seconds = 60;
      options.rate_limit.lock_seconds = 2;
      try {
        server = devcard::start_server(options);
      } catch (const std::exception&) {
      }
    }
    return server;
  };

  auto post_login = [](httplib::Client& client, const std::string& user,
                       const std::string& password) {
    return client.Post("/api/session/login",
                       "{\"username\":\"" + user + "\",\"password\":\"" +
                           password + "\"}",
                       "application/json");
  };

  int failures = 0;
  fs::path data_dir;
  auto server = start("devcard_ratelimit_", address, 18370, true, &data_dir);
  const std::string password = seed_password(data_dir, "root");
  if (!server || password.empty()) {
    std::cerr << "[失败] 起服务或读取初始密码失败，无法校验限流" << std::endl;
    fs::remove_all(data_dir);
    return 1;
  }
  {
    httplib::Client client(address, server->port());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);

    // 1) 阈值内 4 次失败只回 401，不锁。
    for (int i = 0; i < 4; ++i) {
      const auto response = post_login(client, "root", "wrong-password");
      if (!response || response->status != 401) {
        std::cerr << "[失败] 阈值内的失败未被识别为 401："
                  << (response ? response->status : -1) << " "
                  << (response ? response->body : "") << std::endl;
        ++failures;
      }
    }
    // 2) 第 5 次改用正确密码：必须 200（5 次内不影响正常登录），并清零计数。
    {
      const auto response = post_login(client, "root", password);
      if (!response || response->status != 200) {
        std::cerr << "[失败] 阈值内用正确密码登录失败："
                  << (response ? response->status : -1) << " "
                  << (response ? response->body : "") << std::endl;
        ++failures;
      }
    }
    // 3) 同一来源换不同用户名连错 5 次：IP 键到阈值。
    for (int i = 0; i < 5; ++i) {
      const auto response = post_login(client, "ghost" + std::to_string(i), "wrong");
      if (!response || response->status != 401) {
        std::cerr << "[失败] 第 " << i + 1 << " 次失败未回 401："
                  << (response ? response->status : -1) << std::endl;
        ++failures;
      }
    }
    // 4) 锁定期内即使密码正确也 429，并带 Retry-After。
    {
      const auto response = post_login(client, "root", password);
      const bool limited =
          response && response->status == 429 &&
          response->body.find("too_many_requests") != std::string::npos &&
          !response->get_header_value("Retry-After").empty();
      if (!limited) {
        std::cerr << "[失败] 锁定期内正确密码未被 429 拒绝（或缺 Retry-After）："
                  << (response ? response->status : -1) << " "
                  << (response ? response->body : "") << std::endl;
        ++failures;
      }
    }
    // 5) 锁定过期后能登：证明锁是限时的，不是永久拉黑。
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    {
      const auto response = post_login(client, "root", password);
      if (!response || response->status != 200) {
        std::cerr << "[失败] 锁定过期后仍登录失败："
                  << (response ? response->status : -1) << " "
                  << (response ? response->body : "") << std::endl;
        ++failures;
      }
    }
    // 6) 用户名键独立生效：同一账号连错 5 次后，第 6 次必须 429 而不是 423。
    //    （账号库里也有 5 次锁 15 分钟的老机制；限流必须先拒，否则这条会退回 423。）
    for (int i = 0; i < 5; ++i) {
      post_login(client, "root", "wrong-password");
    }
    {
      const auto response = post_login(client, "root", password);
      if (!response || response->status != 429) {
        std::cerr << "[失败] 用户名键未生效：期望 429，实得 "
                  << (response ? response->status : -1) << " "
                  << (response ? response->body : "") << std::endl;
        ++failures;
      }
    }
  }
  server.reset();
  fs::remove_all(data_dir);

  // 7) 本机地址不受限：回环来源连错 8 次，一次 429 都不许有。
  {
    fs::path loop_dir;
    auto loop_server =
        start("devcard_ratelimit_loop_", "127.0.0.1", 18395, true, &loop_dir);
    if (!loop_server) {
      std::cerr << "[失败] 起回环服务失败，无法校验「本机地址不受限」" << std::endl;
      fs::remove_all(loop_dir);
      ++failures;
    } else {
      httplib::Client client("127.0.0.1", loop_server->port());
      client.set_connection_timeout(5, 0);
      client.set_read_timeout(5, 0);
      for (int i = 0; i < 8; ++i) {
        const auto response = post_login(client, "ghost" + std::to_string(i), "wrong");
        if (!response || response->status == 429) {
          std::cerr << "[失败] 本机地址被限流了："
                    << (response ? response->status : -1) << " "
                    << (response ? response->body : "") << std::endl;
          ++failures;
        }
      }
      loop_server.reset();
      fs::remove_all(loop_dir);
    }
  }

  // 8) 关掉开关后不限：同样从非回环来源连错 8 次，不许出现 429。
  {
    fs::path off_dir;
    auto off_server = start("devcard_ratelimit_off_", address, 18415, false, &off_dir);
    if (!off_server) {
      std::cerr << "[失败] 起服务失败，无法校验「关开关后不限」" << std::endl;
      fs::remove_all(off_dir);
      ++failures;
    } else {
      httplib::Client client(address, off_server->port());
      client.set_connection_timeout(5, 0);
      client.set_read_timeout(5, 0);
      for (int i = 0; i < 8; ++i) {
        const auto response = post_login(client, "ghost" + std::to_string(i), "wrong");
        if (!response || response->status == 429) {
          std::cerr << "[失败] 关闭限流后仍出现 429："
                    << (response ? response->status : -1) << std::endl;
          ++failures;
        }
      }
      off_server.reset();
      fs::remove_all(off_dir);
    }
  }

  if (failures == 0) {
    std::cout << "限流自检通过：阈值内 401、第 6 次 429 带 Retry-After、锁定期内"
                 "正确密码也 429、锁定过后能登、本机地址不限、关开关后不限"
              << std::endl;
  }
  return failures;
}

// 阶段二判据：app-started.marker 必须真落地；apply-state.json 停在 installing
// 且超过阈值时，重启后的接口必须报 interrupted——「协助器被杀」在磁盘上留下的
// 正是这两样证据（state=installing、没有 install-result.json），所以这条用例
// 复现的是可复现的持久化条件，而不是手点一次。
int check_apply_state() {
  namespace fs = std::filesystem;
#ifdef _WIN32
  _putenv_s("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR);
#else
  setenv("DEVCARD_FONT_DIR", DEVCARD_WEB_FONTS_DIR, 1);
#endif
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path data_dir =
      fs::temp_directory_path() / ("devcard_applystate_" + std::to_string(stamp));

  auto start = [&data_dir](int base) {
    std::unique_ptr<devcard::ServerHandle> server;
    for (int port = base; port <= base + 20 && !server; ++port) {
      devcard::ServerOptions options;
      options.data_dir = data_dir;
      options.port = port;
      try {
        server = devcard::start_server(options);
      } catch (const std::exception&) {
      }
    }
    return server;
  };

  int failures = 0;
  auto server = start(18440);
  if (!server) {
    std::cerr << "[失败] 起服务失败，无法校验阶段二判据" << std::endl;
    fs::remove_all(data_dir);
    return 1;
  }
  if (!fs::exists(data_dir / "app-started.marker")) {
    std::cerr << "[失败] app-started.marker 未落盘：启动握手判据无法成立"
              << std::endl;
    ++failures;
  }

  // 登录并完成强制改密，后续接口才可达。
  std::string password = seed_password(data_dir, "root");
  auto login = [&](httplib::Client& client, const std::string& pwd,
                   std::string* cookie) {
    const auto response = client.Post(
        "/api/session/login",
        "{\"username\":\"root\",\"password\":\"" + pwd + "\"}", "application/json");
    if (!response || response->status != 200) return false;
    const std::string raw = response->get_header_value("Set-Cookie");
    *cookie = raw.substr(0, raw.find(';'));
    return !cookie->empty();
  };

  {
    httplib::Client client("127.0.0.1", server->port());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    std::string cookie;
    if (!login(client, password, &cookie)) {
      std::cerr << "[失败] 初始账号登录失败" << std::endl;
      ++failures;
    } else {
      client.set_default_headers(
          {{"Cookie", cookie}, {"X-Requested-With", "DevCard"}});
      const auto change = client.Post(
          "/api/users/password", R"({"new_password":"ApplyState123"})",
          "application/json");
      if (!change || change->status != 200) {
        std::cerr << "[失败] 首次改密失败，无法继续校验安装状态" << std::endl;
        ++failures;
      }
      password = "ApplyState123";

      // 刚写入的 installing：不该被当成中断。
      fs::create_directories(data_dir / "updates");
      {
        std::ofstream out(data_dir / "updates" / "apply-state.json",
                          std::ios::binary | std::ios::trunc);
        out << R"({"state":"installing","detail":"用例写入","updated_at":""})";
      }
      const auto fresh = client.Get("/api/update/install-result");
      if (!fresh || fresh->status != 200 ||
          fresh->body.find("\"installing\"") == std::string::npos) {
        std::cerr << "[失败] 刚写入的 installing 未被识别为进行中："
                  << (fresh ? fresh->status : -1) << " "
                  << (fresh ? fresh->body : "") << std::endl;
        ++failures;
      }
    }
  }

  // 把状态文件时间拨回 1 小时，模拟「协助器被杀、机器重启过后才起来」。
  {
    std::error_code ec;
    const fs::path state_file = data_dir / "updates" / "apply-state.json";
    fs::last_write_time(state_file,
                        fs::file_time_type::clock::now() - std::chrono::hours(1),
                        ec);
    if (ec) {
      std::cerr << "[失败] 无法回拨状态文件时间：" << ec.message() << std::endl;
      ++failures;
    }
  }
  server.reset();
  server = start(18470);
  if (!server) {
    std::cerr << "[失败] 重启服务失败，无法校验中断判据" << std::endl;
    fs::remove_all(data_dir);
    return failures + 1;
  }
  {
    httplib::Client client("127.0.0.1", server->port());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    std::string cookie;
    if (!login(client, password, &cookie)) {
      std::cerr << "[失败] 重启后登录失败" << std::endl;
      ++failures;
    } else {
      client.set_default_headers(
          {{"Cookie", cookie}, {"X-Requested-With", "DevCard"}});
      const auto result = client.Get("/api/update/install-result");
      const bool interrupted =
          result && result->status == 200 &&
          result->body.find("\"interrupted\"") != std::string::npos;
      if (!interrupted) {
        std::cerr << "[失败] 停在 installing 且超阈值时未报 interrupted："
                  << (result ? result->status : -1) << " "
                  << (result ? result->body : "") << std::endl;
        ++failures;
      }
    }
  }
  server.reset();
  fs::remove_all(data_dir);

  if (failures == 0) {
    std::cout << "阶段二判据自检通过：app-started.marker 已落盘；"
                 "installing 超阈值经重启后报 interrupted"
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
      // 重写历史前的提交（已不可引用）的七项 + 下载路由
      {"endpoint_not_allowed", 400}, {"no_update", 400},
      {"update_disabled", 403},      {"update_busy", 409},
      {"manifest_invalid", 502},     {"download_failed", 502},
      {"checksum_mismatch", 502},
      // 同类洞：未登录不是服务端错误
      {"unauthorized", 401},
      // 限流单独一项：不能与 401/403 混在一起，客户端要靠 429 + Retry-After 等待
      {"too_many_requests", 429},
  };
  for (const Expected& item : expected) {
    const int actual = devcard::http_status_for(item.code);
    if (actual != item.status) {
      std::cerr << "[失败] " << item.code << " 期望 " << item.status << "，实得 "
                << actual << std::endl;
      ++failures;
    }
  }

  // 源码扫描：server/src 与 core/src 里直接 throw 的码，要么在清单里，要么是启动期错误。
  // 扫整个目录而不是单个 server.cpp：config.cpp / platform_paths.cpp / apply_update.cpp
  // 也会抛 AppError，只扫一个文件等于给它们开免责通道。
  // 扫描 AppError(<空白>"code" ...)：括号与引号之间允许换行。
  // 之前固定匹配 AppError(" 会漏掉换行写法（port_in_use、install_unsupported、
  // bad_version、bad_company_name 都这么写），等于给这类码开了后门。
  const std::string needle = "AppError(";
  const auto scan_codes = [&](const std::string& text, const std::string& label,
                              const std::set<std::string>& exempt) {
    std::size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
      at += needle.size();
      while (at < text.size() &&
             (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' ||
              text[at] == '\n')) {
        ++at;
      }
      if (at >= text.size() || text[at] != '"') continue;
      const std::size_t stop = text.find('"', at + 1);
      if (stop == std::string::npos) break;
      const std::string code = text.substr(at + 1, stop - at - 1);
      at = stop + 1;
      if (exempt.count(code)) continue;
      if (!listed.count(code)) {
        std::cerr << "[失败] " << label << " 抛出了未登记的错误码 \"" << code
                  << "\"：请同时加进 route_error_codes() 与 status_for 的表"
                  << std::endl;
        ++failures;
      }
    }
  };
  // 启动阶段（还没开始服务）抛的码不经过路由，不需要 HTTP 状态。
  // bad_config / bad_listen / data_dir_unwritable 来自配置解析与数据目录选择，
  // 同样发生在开始服务之前。
  const std::set<std::string> startup_only = {
      "web_assets",       "data_dir",          "font_missing",
      "font_corrupt",     "port_in_use",       "instance_in_use",
      "app_started_marker", "bad_config",      "bad_listen",
      "data_dir_unwritable"};
  // core/src 只豁免启动期码（建库、打开库、迁移），它们在开始服务之前就结束了。
  const std::set<std::string> core_startup_only = {
      "db_dir", "db_open", "schema_newer", "migration_checksum"};

  std::error_code dir_error;
  const auto scan_dir = [&](const std::string& root,
                            const std::set<std::string>& exempt) {
    dir_error.clear();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, dir_error)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
        continue;
      }
      std::ifstream file_in(entry.path(), std::ios::binary);
      if (!file_in) {
        std::cerr << "[失败] 读不到 " << entry.path().string() << std::endl;
        ++failures;
        continue;
      }
      const std::string text{std::istreambuf_iterator<char>(file_in),
                             std::istreambuf_iterator<char>()};
      scan_codes(text, entry.path().filename().string(), exempt);
    }
  };
  scan_dir(DEVCARD_SERVER_SOURCE_DIR, startup_only);
  scan_dir(DEVCARD_CORE_SOURCE_DIR, core_startup_only);

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
    std::cout << "内嵌前端自检通过：index.html / styles.css / app.js / logo.jpg / favicon.ico 与源文件一致"
              << "（index.html " << devcard::embedded_index_html_size() << " B, styles.css "
              << devcard::embedded_styles_css_size() << " B, app.js "
              << devcard::embedded_app_js_size() << " B）" << std::endl;
  }
  if (check_fonts() != 0) {
    code = 1;
  }
  if (check_font_gate() != 0) {
    code = 1;
  }
  if (check_csp() != 0) {
    code = 1;
  }
  if (check_update_guards() != 0) {
    code = 1;
  }
  if (check_rate_limit() != 0) {
    code = 1;
  }
  if (check_apply_state() != 0) {
    code = 1;
  }
  if (check_status_map() != 0) {
    code = 1;
  }
  return code;
}
