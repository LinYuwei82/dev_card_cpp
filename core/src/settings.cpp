#include "devcard/settings.h"

#include <vector>

#include "devcard/dao/setting_dao.h"

namespace devcard {
namespace {

std::vector<unsigned int> decode_utf8(std::string_view text) {
  std::vector<unsigned int> points;
  points.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    unsigned int point = 0;
    std::size_t extra = 0;
    if (lead < 0x80) {
      point = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      point = lead & 0x1F;
      extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      point = lead & 0x0F;
      extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      point = lead & 0x07;
      extra = 3;
    } else {
      ++i;
      continue;
    }
    if (extra > 0 && i + extra >= text.size()) break;
    for (std::size_t k = 1; k <= extra; ++k) {
      point = (point << 6) |
              (static_cast<unsigned char>(text[i + k]) & 0x3F);
    }
    points.push_back(point);
    i += extra + 1;
  }
  return points;
}

double codepoint_width(unsigned int point) {
  return point < 0x80 ? 0.5 : 1.0;
}

}  // namespace

std::string get_setting(const Db& db, std::string_view key) {
  return SettingDao().get(db, key);
}

bool has_setting(const Db& db, std::string_view key) {
  return SettingDao().has(db, key);
}

void set_setting(sqlite3* conn, std::string_view key, std::string_view value) {
  SettingDao().set(conn, key, value);
}

std::string company_name(const Db& db) {
  const std::string value = get_setting(db, "print_company_name");
  return value.empty() ? std::string(kDefaultCompanyName) : value;
}

double company_name_width(std::string_view text) {
  double width = 0;
  for (unsigned int point : decode_utf8(text)) width += codepoint_width(point);
  return width;
}

std::string truncate_company_name(std::string_view text) {
  double width = 0;
  std::size_t bytes = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::size_t length = 1;
    unsigned int point = lead;
    if ((lead & 0xE0) == 0xC0) {
      length = 2;
      point = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      point = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      point = lead & 0x07;
    }
    if (i + length > text.size()) break;
    for (std::size_t k = 1; k < length; ++k) {
      point = (point << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    }
    const double w = codepoint_width(point);
    if (width + w > kCompanyNameMaxWidth + 1e-9) break;
    width += w;
    i += length;
    bytes = i;
  }
  return std::string(text.substr(0, bytes));
}

std::string company_print_name(const Db& db) {
  return truncate_company_name(company_name(db));
}

}  // namespace devcard
