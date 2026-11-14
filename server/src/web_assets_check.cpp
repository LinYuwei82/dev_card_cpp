#include "web_assets.h"

#include <string>
#include <string_view>

#include "devcard/sha256.h"

namespace devcard {
namespace {

struct Asset {
  const char* name;
  const char* data;
  EmbeddedAsset asset;
};

std::string check_one(const Asset& item) {
  const std::string_view data(item.data);
  const std::size_t expected_size = embedded_source_size(item.asset);
  if (data.size() != expected_size) {
    return std::string(item.name) + "：内嵌 " + std::to_string(data.size()) +
           " 字节，源文件 " + std::to_string(expected_size) + " 字节";
  }
  const std::string actual = sha256_hex(data);
  const std::string expected = embedded_source_sha256(item.asset);
  if (actual != expected) {
    return std::string(item.name) + "：内嵌内容 SHA256 " + actual +
           " 与源文件 " + expected + " 不一致";
  }
  return {};
}

}  // namespace

std::string embedded_assets_error() {
  const Asset assets[] = {
      {"index.html", embedded_index_html(), EmbeddedAsset::kIndexHtml},
      {"styles.css", embedded_styles_css(), EmbeddedAsset::kStylesCss},
      {"app.js", embedded_app_js(), EmbeddedAsset::kAppJs},
  };
  for (const Asset& asset : assets) {
    std::string error = check_one(asset);
    if (!error.empty()) return error;
  }

  // logo.jpg 内嵌的是十六进制串，要先转回字节再比哈希。
  const std::string_view hex(embedded_logo_jpg_hex());
  if (hex.size() % 2 != 0 || hex.size() / 2 != embedded_source_size(EmbeddedAsset::kLogoJpg)) {
    return "logo.jpg：内嵌十六进制长度 " + std::to_string(hex.size()) +
           " 与源文件字节数 " +
           std::to_string(embedded_source_size(EmbeddedAsset::kLogoJpg)) + " 不符";
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int high = nibble(hex[i]);
    const int low = nibble(hex[i + 1]);
    if (high < 0 || low < 0) return "logo.jpg：内嵌十六进制串含非法字符";
    bytes.push_back(static_cast<char>((high << 4) | low));
  }
  if (sha256_hex(bytes) != embedded_logo_jpg_sha256()) {
    return "logo.jpg：内嵌内容 SHA256 与源文件不一致";
  }
  return {};
}

}  // namespace devcard
