#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace devcard {

// 子集字体实际能印的码位集合，直接从随包 TTF 的 cmap 表解析。
// 用途是「哪些字能印」的唯一判据：不接受手写清单，换字体/换字符集时它自动跟着变。
// 只支持 TrueType 的 cmap 格式 4（BMP）与格式 12（全平面），够覆盖 Noto 子集。
class FontCoverage {
 public:
  // 解析失败返回空覆盖（covers() 全为 false），错误信息留在 error()。
  static FontCoverage parse(const std::string& ttf);

  bool empty() const { return ranges_.empty(); }
  std::size_t count() const { return count_; }
  const std::string& error() const { return error_; }

  bool covers(std::uint32_t codepoint) const;
  // [[start, end], ...]，闭区间，升序。
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges() const { return ranges_; }

 private:
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges_;
  std::size_t count_ = 0;
  std::string error_;
};

}  // namespace devcard
