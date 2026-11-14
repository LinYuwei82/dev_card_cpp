#include "font_coverage.h"

#include <algorithm>
#include <utility>

namespace devcard {
namespace {

std::uint16_t be16(const std::string& data, std::size_t offset) {
  if (offset + 2 > data.size()) return 0;
  return static_cast<std::uint16_t>((static_cast<unsigned char>(data[offset]) << 8) |
                                    static_cast<unsigned char>(data[offset + 1]));
}

std::uint32_t be32(const std::string& data, std::size_t offset) {
  if (offset + 4 > data.size()) return 0;
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

constexpr std::uint32_t kNoGlyph = 0;

// 找出 cmap 表里最适合的子表：优先格式 12（全平面），否则格式 4（BMP）。
std::size_t find_subtable(const std::string& data, std::size_t cmap_offset,
                          std::uint16_t* format_out) {
  const std::size_t records = cmap_offset + 4;
  const std::size_t table_count = be16(data, cmap_offset + 2);
  std::size_t best = 0;
  int best_score = -1;
  for (std::size_t i = 0; i < table_count; ++i) {
    const std::size_t record = records + i * 8;
    const std::uint16_t platform = be16(data, record);
    const std::uint16_t encoding = be16(data, record + 2);
    const std::size_t sub = cmap_offset + be32(data, record + 4);
    const std::uint16_t format = be16(data, sub);
    if (format != 4 && format != 12) continue;
    int score = 0;
    if (platform == 3 && encoding == 10) score = 3;        // Windows UCS-4
    else if (platform == 0 && (encoding == 4 || encoding == 6)) score = 3;
    else if (platform == 3 && encoding == 1) score = 2;    // Windows BMP
    else if (platform == 0) score = 1;                     // Unicode 平台
    if (score > best_score) {
      best_score = score;
      best = sub;
    }
  }
  if (best_score < 0) return 0;
  *format_out = be16(data, best);
  return best;
}

void collect_format4(const std::string& data, std::size_t sub, std::vector<std::uint32_t>* out) {
  const std::size_t seg_count = be16(data, sub + 6) / 2;
  const std::size_t end_codes = sub + 14;
  const std::size_t start_codes = end_codes + seg_count * 2 + 2;
  const std::size_t id_deltas = start_codes + seg_count * 2;
  const std::size_t id_range_offsets = id_deltas + seg_count * 2;
  for (std::size_t i = 0; i < seg_count; ++i) {
    const std::uint32_t end = be16(data, end_codes + i * 2);
    const std::uint32_t start = be16(data, start_codes + i * 2);
    const std::uint16_t delta = be16(data, id_deltas + i * 2);
    const std::uint16_t range_offset = be16(data, id_range_offsets + i * 2);
    if (start == 0xFFFF && end == 0xFFFF) continue;
    for (std::uint32_t cp = start; cp <= end && cp != 0xFFFF; ++cp) {
      std::uint32_t glyph = 0;
      if (range_offset == 0) {
        glyph = (cp + delta) & 0xFFFF;
      } else {
        const std::size_t slot = id_range_offsets + i * 2 + range_offset + (cp - start) * 2;
        glyph = be16(data, slot);
        if (glyph != 0) glyph = (glyph + delta) & 0xFFFF;
      }
      if (glyph != kNoGlyph) out->push_back(cp);
    }
  }
}

void collect_format12(const std::string& data, std::size_t sub, std::vector<std::uint32_t>* out) {
  const std::size_t groups = be32(data, sub + 12);
  const std::size_t base = sub + 16;
  for (std::size_t i = 0; i < groups; ++i) {
    const std::uint32_t start = be32(data, base + i * 12);
    const std::uint32_t end = be32(data, base + i * 12 + 4);
    const std::uint32_t start_glyph = be32(data, base + i * 12 + 8);
    if (end < start || end - start > 0x20000) continue;
    for (std::uint32_t cp = start; cp <= end; ++cp) {
      const std::uint32_t glyph = start_glyph == 0 ? (cp == start ? 0 : 1) : 1;
      if (glyph != kNoGlyph) out->push_back(cp);
    }
  }
}

}  // namespace

FontCoverage FontCoverage::parse(const std::string& ttf) {
  FontCoverage coverage;
  if (ttf.size() < 12) {
    coverage.error_ = "字体文件过短";
    return coverage;
  }
  const std::size_t tables = be16(ttf, 4);
  std::size_t cmap = 0;
  for (std::size_t i = 0; i < tables; ++i) {
    const std::size_t record = 12 + i * 16;
    if (record + 16 > ttf.size()) break;
    if (ttf.compare(record, 4, "cmap") == 0) {
      cmap = be32(ttf, record + 8);
      break;
    }
  }
  if (cmap == 0) {
    coverage.error_ = "找不到 cmap 表";
    return coverage;
  }
  std::uint16_t format = 0;
  const std::size_t sub = find_subtable(ttf, cmap, &format);
  if (sub == 0) {
    coverage.error_ = "cmap 里没有格式 4/12 的子表";
    return coverage;
  }
  std::vector<std::uint32_t> codepoints;
  if (format == 12) {
    collect_format12(ttf, sub, &codepoints);
  } else {
    collect_format4(ttf, sub, &codepoints);
  }
  std::sort(codepoints.begin(), codepoints.end());
  codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
  for (std::uint32_t cp : codepoints) {
    if (!coverage.ranges_.empty() && cp == coverage.ranges_.back().second + 1) {
      coverage.ranges_.back().second = cp;
    } else {
      coverage.ranges_.emplace_back(cp, cp);
    }
  }
  coverage.count_ = codepoints.size();
  return coverage;
}

bool FontCoverage::covers(std::uint32_t codepoint) const {
  std::size_t low = 0;
  std::size_t high = ranges_.size();
  while (low < high) {
    const std::size_t mid = (low + high) / 2;
    if (codepoint < ranges_[mid].first) {
      high = mid;
    } else if (codepoint > ranges_[mid].second) {
      low = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

}  // namespace devcard
