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
#include <string>

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
  return code;
}
