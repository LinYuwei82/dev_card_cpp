#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace devcard {

enum class ImageFormat { kUnknown, kPng, kJpeg };

struct ImageInfo {
  ImageFormat format = ImageFormat::kUnknown;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// 只认 PNG / JPEG（SVG 与其它格式一律 kUnknown），并读出像素尺寸。
ImageInfo inspect_image(std::string_view bytes);

// 卡片头部图标区 8.8mm × 8.8mm，300 dpi 需要 104×104 像素。
inline constexpr std::uint32_t kLogoMinPixels = 104;
// 图标允许的宽高比（宽/高），超出视为横幅等非图标素材。
inline constexpr double kLogoMinAspect = 0.6;
inline constexpr double kLogoMaxAspect = 1.67;

}  // namespace devcard
