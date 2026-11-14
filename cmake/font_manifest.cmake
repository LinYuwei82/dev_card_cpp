# 生成字体清单头文件：名称、URL、字节数、SHA256 全部取自 web/fonts/ 下的真实文件。
#
# 为什么要生成而不是手写：这两个数原先写死在 web/app.js 与 server 的启动逻辑里，
# 重新子集一次忘了同步，导出就会永久中止，报「字节数不符」而真正的原因是清单过期。
# 现在构建期直接从源文件读，前端与服务器共用同一份常量，来源唯一。
#
# 判据用 SHA256（不只比字节数）：重子集后大小碰巧相同、内容变了，只比大小会判「通过」，
# 然后拿错字体去排版。
#
# 参数：
#   FONT_DIR  字体目录（通常是 web/fonts）
#   OUTPUT    生成的头文件路径

if(NOT FONT_DIR)
  message(FATAL_ERROR "font_manifest：缺少 FONT_DIR")
endif()
if(NOT OUTPUT)
  message(FATAL_ERROR "font_manifest：缺少 OUTPUT")
endif()

set(font_names
    "NotoSansSC-Regular.subset.ttf"
    "NotoSansSC-Bold.subset.ttf")

set(entries "")
foreach(name IN LISTS font_names)
  set(path "${FONT_DIR}/${name}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "font_manifest：缺少字体 ${path}（随包字体必须入库）")
  endif()
  file(SIZE "${path}" bytes)
  file(SHA256 "${path}" hash)
  if(bytes LESS 100000)
    message(FATAL_ERROR "font_manifest：${path} 只有 ${bytes} 字节，不可能是一份完整子集")
  endif()
  string(APPEND entries
         "    {\"${name}\", \"/fonts/${name}\", ${bytes}, \"${hash}\"},\n")
endforeach()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

file(WRITE "${OUTPUT}"
"// 由 cmake/font_manifest.cmake 生成，不要手改。
// 数据取自 web/fonts/ 的真实文件：字节数与 SHA256 必须在构建期一致，
// 服务器启动时按这两个值校验随包字体，前端按同一份值校验 /fonts/ 响应。
#pragma once

#include <cstddef>
#include <cstdint>

namespace devcard {

struct EmbeddedFontSpec {
  const char* name;
  const char* url;
  std::uint64_t bytes;
  const char* sha256;
};

inline constexpr EmbeddedFontSpec kEmbeddedFonts[] = {
${entries}};

inline constexpr std::size_t kEmbeddedFontCount =
    sizeof(kEmbeddedFonts) / sizeof(kEmbeddedFonts[0]);

}  // namespace devcard
")
