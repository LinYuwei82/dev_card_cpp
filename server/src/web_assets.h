#pragma once

#include <cstddef>
#include <string>

namespace devcard {

enum class EmbeddedAsset {
  kIndexHtml,
  kStylesCss,
  kAppJs,
  kLogoJpg,
  kFaviconIco,
};

const char* embedded_index_html();
const char* embedded_styles_css();
const char* embedded_app_js();

// 打印卡片 Logo，十六进制字符串（构建时由 web/logo.jpg 生成）。
const char* embedded_logo_jpg_hex();
// 浏览器 favicon，十六进制字符串（构建时由 web/favicon.ico 生成）。
const char* embedded_favicon_ico_hex();

// 由 cmake/embed_web.cmake 一并写入的期望值：直读 web/ 源文件得到的字节数与 SHA256。
// 用途是抓 MSVC C2026 的「静默截断」——截断后页面仍能打开，只是内容少了几个字符。
std::size_t embedded_index_html_size();
std::size_t embedded_styles_css_size();
std::size_t embedded_app_js_size();

const char* embedded_index_html_sha256();
const char* embedded_styles_css_sha256();
const char* embedded_app_js_sha256();
const char* embedded_logo_jpg_sha256();
const char* embedded_favicon_ico_sha256();

std::size_t embedded_source_size(EmbeddedAsset asset);
const char* embedded_source_sha256(EmbeddedAsset asset);

// Web 图标 / manifest 等静态资源（favicon PNG、PWA 图标、Touch 图标、OG 图、manifest）。
// 与上面几个固定资源不同，这些只按路径取用，不参与自检哈希。
struct StaticAsset {
  const char* path;
  const char* mime;
  const char* hex;
};

const StaticAsset* embedded_static_assets();
std::size_t embedded_static_asset_count();

// 前端 ES 模块（web/js/*.js）：路径形如 /js/core.js，随 app.js 一起内嵌。
// 每个模块都带源文件字节数与 SHA256，运行时与 ctest 逐份核对。
struct JsModule {
  const char* path;
  const char* data;
  std::size_t size;
  const char* sha256;
};

const JsModule* embedded_js_modules();
std::size_t embedded_js_module_count();

// 逐个核对内嵌内容与源文件是否一致；全部一致返回空串，否则返回第一条差异说明。
std::string embedded_assets_error();

}  // namespace devcard
