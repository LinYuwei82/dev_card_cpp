#include "devcard/image_meta.h"

#include <cstdint>

namespace devcard {
namespace {

std::uint32_t read_be32(const unsigned char* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

std::uint32_t read_be16(const unsigned char* data) {
  return (static_cast<std::uint32_t>(data[0]) << 8) |
         static_cast<std::uint32_t>(data[1]);
}

bool is_png(std::string_view bytes) {
  static const unsigned char kSignature[8] = {0x89, 0x50, 0x4E, 0x47,
                                              0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() < 24) return false;
  for (std::size_t i = 0; i < 8; ++i) {
    if (static_cast<unsigned char>(bytes[i]) != kSignature[i]) return false;
  }
  return bytes.compare(12, 4, "IHDR") == 0;
}

// JPEG：跳过段头，读到 SOFn 时取宽高。
bool jpeg_size(std::string_view bytes, std::uint32_t* width,
               std::uint32_t* height) {
  if (bytes.size() < 4) return false;
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  if (data[0] != 0xFF || data[1] != 0xD8) return false;

  std::size_t pos = 2;
  while (pos + 4 <= bytes.size()) {
    if (data[pos] != 0xFF) {
      ++pos;
      continue;
    }
    while (pos + 1 < bytes.size() && data[pos + 1] == 0xFF) ++pos;
    if (pos + 1 >= bytes.size()) return false;
    const unsigned char marker = data[pos + 1];
    pos += 2;
    if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
        (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (pos + 2 > bytes.size()) return false;
    const std::uint32_t length = read_be16(data + pos);
    if (length < 2 || pos + length > bytes.size()) return false;
    const bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                        marker != 0xC8 && marker != 0xCC;
    if (is_sof) {
      if (length < 7) return false;
      *height = read_be16(data + pos + 3);
      *width = read_be16(data + pos + 5);
      return *width > 0 && *height > 0;
    }
    if (marker == 0xDA) return false;  // 进入扫描数据，不再有 SOF
    pos += length;
  }
  return false;
}

}  // namespace

ImageInfo inspect_image(std::string_view bytes) {
  ImageInfo info;
  if (is_png(bytes)) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    info.format = ImageFormat::kPng;
    info.width = read_be32(data + 16);
    info.height = read_be32(data + 20);
    if (info.width == 0 || info.height == 0) info.format = ImageFormat::kUnknown;
    return info;
  }
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (jpeg_size(bytes, &width, &height)) {
    info.format = ImageFormat::kJpeg;
    info.width = width;
    info.height = height;
  }
  return info;
}

}  // namespace devcard
