// 内嵌前端自检：确认 web_assets.cpp 里的字符串与 web/ 源文件逐字节一致。
// 这条专抓 MSVC C2026 的静默截断——截断后页面仍能打开，功能却悄悄少了。
#include <iostream>

#include "web_assets.h"

int main() {
  const std::string error = devcard::embedded_assets_error();
  if (!error.empty()) {
    std::cerr << "[失败] 内嵌前端与源文件不一致：" << error << std::endl;
    return 1;
  }
  std::cout << "内嵌前端自检通过：index.html / styles.css / app.js / logo.jpg 与源文件一致"
            << "（index.html " << devcard::embedded_index_html_size() << " B, styles.css "
            << devcard::embedded_styles_css_size() << " B, app.js "
            << devcard::embedded_app_js_size() << " B）" << std::endl;
  return 0;
}
