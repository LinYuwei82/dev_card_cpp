#include "routes.h"

#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/error.h"
#include "http_json.h"
#include "logo_store.h"
#include "web_assets.h"

namespace devcard::detail {

void register_static_routes(ServerState& state) {
  using nlohmann::json;

  state.server.Get("/", [&state](const httplib::Request&, httplib::Response& response) {
    // 开发态（--web-dir）每次请求都重读磁盘，改完刷新即生效。
    std::string html = state.web_dir.empty()
                           ? std::string(embedded_index_html())
                           : read_file_bytes(state.web_dir / "index.html");
    if (html.empty()) {
      throw AppError("not_found", "前端文件不存在：index.html");
    }
    // 本地窗口模式注入一次性令牌供壳自动登录；网络模式绝不注入：
    // 否则任何能打开首页的人都能拿到本地自动登录令牌。
    if (!state.network) {
      const std::string meta = "<meta name=\"devcard-local-token\" content=\"" +
                               state.local_token + "\">";
      const std::size_t head = html.find("</head>");
      if (head != std::string::npos) html.insert(head, meta + "\n");
    }
    set_asset(response, html, "text/html; charset=utf-8");
  });
  state.server.Get("/styles.css", [&state](const httplib::Request&,
                                           httplib::Response& response) {
    std::string css = state.web_dir.empty() ? std::string(embedded_styles_css())
                                            : read_file_bytes(state.web_dir / "styles.css");
    if (css.empty()) throw AppError("not_found", "前端文件不存在：styles.css");
    set_asset(response, css, "text/css; charset=utf-8");
  });
  state.server.Get("/app.js", [&state](const httplib::Request&,
                                       httplib::Response& response) {
    std::string js = state.web_dir.empty() ? std::string(embedded_app_js())
                                           : read_file_bytes(state.web_dir / "app.js");
    if (js.empty()) throw AppError("not_found", "前端文件不存在：app.js");
    set_asset(response, js, "application/javascript; charset=utf-8");
  });
  // 前端 ES 模块（app.js 里 import 的那些）：整表按内嵌路径注册，开发态读磁盘。
  // 拆 server.cpp 时漏掉这段会让 app.js 的 import 全 404，页面只剩空白背景。
  for (std::size_t i = 0; i < embedded_js_module_count(); ++i) {
    const JsModule* module = &embedded_js_modules()[i];
    state.server.Get(
        module->path,
        [&state, module](const httplib::Request&, httplib::Response& response) {
          std::string js = state.web_dir.empty()
                               ? std::string(module->data)
                               : read_file_bytes(state.web_dir / (module->path + 1));
          if (js.empty()) {
            throw AppError("not_found", "前端文件不存在：" + std::string(module->path));
          }
          set_asset(response, js, "application/javascript; charset=utf-8");
        });
  }

  // Web 图标与 manifest：开发态（--web-dir）优先读磁盘，打包后回落到内嵌。
  // 这些路径都在 /api/ 之外，认证中间件不介入。
  for (std::size_t i = 0; i < embedded_static_asset_count(); ++i) {
    // 按值捕获指针：asset 是循环内的引用变量，捕获它的引用会在下一轮迭代后悬垂。
    const StaticAsset* asset = &embedded_static_assets()[i];
    state.server.Get(
        asset->path,
        [&state, asset](const httplib::Request&, httplib::Response& response) {
          std::string bytes;
          if (!state.web_dir.empty()) {
            // asset->path 以 / 开头，直接拼会覆盖 web_dir，必须先去掉。
            bytes = read_file_bytes(state.web_dir / (asset->path + 1));
          }
          if (bytes.empty()) bytes = hex_to_bytes(asset->hex);
          if (bytes.empty()) {
            set_json(response,
                     json{{"error", "not_found"},
                          {"message", std::string(asset->path) + " 不存在"}},
                     404);
            return;
          }
          response.status = 200;
          response.set_header("Content-Type", asset->mime);
          response.set_header("Cache-Control", "public, max-age=86400");
          response.body = std::move(bytes);
        });
  }

  state.server.Get(R"(/fonts/([A-Za-z0-9._-]+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const std::string name = request.matches[1].str();
                     const auto it = state.fonts.find(name);
                     if (it == state.fonts.end()) {
                       set_json(response,
                                json{{"error", "not_found"},
                                     {"message", "字体不存在：" + name}},
                                404);
                       return;
                     }
                     response.status = 200;
                     response.set_header("Content-Type", "font/ttf");
                     // 导出前的门禁资源：它必须在每次导出时被真正取到并校验，
                     // 缓存一天等于把「取不到就中止」这条判据拆掉。本地回环传 4.56 MB 是几毫秒。
                     response.set_header("Cache-Control", "no-store");
                     response.body = it->second;
                   });
  state.server.Get("/logo.jpg", [](const httplib::Request&, httplib::Response& response) {
    response.status = 200;
    response.set_header("Content-Type", "image/jpeg");
    response.set_header("Cache-Control", "public, max-age=86400");
    response.body = logo_jpeg();
  });
  state.server.Get("/favicon.ico",
                   [&state](const httplib::Request&,
                            httplib::Response& response) {
                     std::string bytes;
                     if (!state.web_dir.empty()) {
                       bytes = read_file_bytes(state.web_dir / "favicon.ico");
                     }
                     if (bytes.empty()) bytes = favicon_ico();
                     if (bytes.empty()) {
                       set_json(response,
                                json{{"error", "not_found"},
                                     {"message", "favicon.ico 不存在"}},
                                404);
                       return;
                     }
                     response.status = 200;
                     response.set_header("Content-Type", "image/x-icon");
                     response.set_header("Cache-Control", "no-store");
                     response.body = std::move(bytes);
                   });

}

}  // namespace devcard::detail
