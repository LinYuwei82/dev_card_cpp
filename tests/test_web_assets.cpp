// 内嵌前端自检：确认 web_assets.cpp 里的字符串与 web/ 源文件逐字节一致。
// 这条专抓 MSVC C2026 的静默截断——截断后页面仍能打开，功能却悄悄少了。
//
// 同时校验字体清单：font_manifest.h 里的字节数与 SHA256 是构建期从 web/fonts/
// 读出来的，这里再对着源文件复核一次。它是「先有基准，再谈比对」的那一步——
// 没有这份清单，前端和服务器都只能比字节数，同大小不同内容会被放过去。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "devcard/sha256.h"
#include "font_manifest.h"
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
  return code;
}
