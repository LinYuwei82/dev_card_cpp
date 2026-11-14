#pragma once

namespace devcard {

const char* embedded_index_html();
const char* embedded_styles_css();
const char* embedded_app_js();

// 打印卡片 Logo，十六进制字符串（构建时由 web/logo.jpg 生成）。
const char* embedded_logo_jpg_hex();

}  // namespace devcard
