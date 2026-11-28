#include "logo_store.h"

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "web_assets.h"

namespace devcard::detail {
namespace {

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

}  // namespace

std::string hex_to_bytes(std::string_view hex) {
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(
        static_cast<char>((hex_digit(hex[i]) << 4) | hex_digit(hex[i + 1])));
  }
  return out;
}

bool safe_logo_relative(const std::string& path) {
  return !path.empty() && path.find("..") == std::string::npos &&
         path.front() != '/' && path.front() != '\\';
}

std::string read_file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

const std::string& logo_jpeg() {
  static const std::string bytes = hex_to_bytes(embedded_logo_jpg_hex());
  return bytes;
}

const std::string& favicon_ico() {
  static const std::string bytes = hex_to_bytes(embedded_favicon_ico_hex());
  return bytes;
}

}  // namespace devcard::detail
