# 把 web/ 下的文件生成到 web_assets.cpp。
#
# 两条硬要求：
# 1) 单个字符串字面量不能太长。MSVC 的 C2026 上限约 16 KB，超了会**静默截断**：
#    页面照样能开，功能悄悄没了。所以按 4 KB 分块，用相邻字面量拼接（编译期合并）。
# 2) 必须有自检。分块写法本身不保证没截断，所以把「直读 web/ 源文件内容」的字节数与
#    SHA256 一起写进生成文件，运行时由 server 与 ctest 校验。
#    注意校验的是**源文件内容哈希**，不是整个 .cpp 的哈希——.cpp 里有生成路径等信息，
#    跨机器必然不同，那种哈希只会逼人把自检关掉。

function(emit_text_literal out_var path)
  file(READ "${path}" content)
  file(READ "${path}" content_hex HEX)   # 长度单位与切分单位都以字节为准
  string(LENGTH "${content_hex}" total_hex)
  # 注意：string(LENGTH "${content}") 数的是字符，string(SUBSTRING) 切的是字节，
  # 两者混用会让循环提前结束（尾部被丢），或者在汉字中间切开（C4828）。
  # 这里统一用字节数：HEX 串长度 / 2。
  math(EXPR total_bytes "${total_hex} / 2")
  set(offset 0)
  set(parts "")
  set(joined "")
  set(max_chunk 0)
  while(offset LESS total_bytes)
    math(EXPR take "4096")
    # 若切点后一个字节是 UTF-8 续字节（0x80–0xBF），说明切在了汉字中间：
    # 一个字节一个字节往回退。按字节切开生成的是非 UTF-8 源码，MSVC 直接报 C4828 丢内容。
    while(true)
      math(EXPR cut "${offset} + ${take}")
      if(cut GREATER_EQUAL total_bytes)
        break()
      endif()
      math(EXPR cut_hex "${cut} * 2")
      string(SUBSTRING "${content_hex}" ${cut_hex} 2 next_hex)
      math(EXPR next_byte "0x${next_hex}")
      if(next_byte GREATER_EQUAL 128 AND next_byte LESS 192)
        math(EXPR take "${take} - 1")
      else()
        break()
      endif()
    endwhile()
    if(take LESS 1)
      set(take 1)
    endif()
    string(SUBSTRING "${content}" ${offset} ${take} chunk)
    string(LENGTH "${chunk}" chunk_len)
    if(chunk_len GREATER max_chunk)
      set(max_chunk ${chunk_len})
    endif()
    string(APPEND parts "R\"DEVCARD(${chunk})DEVCARD\"\n")
    string(APPEND joined "${chunk}")
    math(EXPR offset "${offset} + ${take}")
  endwhile()

  # 构建期自检：生成器本身必须自洽，不能指望运行期测试兜底。
  # 1) 拼回来的内容字节数与哈希要和源文件一致（抓截断/漏字节）；
  # 2) 每块不超过 4 KB（远离 MSVC C2026 的 16 KB 上限）；
  # 3) 切点不能落在 UTF-8 多字节中间，否则生成的是非法 UTF-8，MSVC 按 C4828 丢内容。
  string(LENGTH "${joined}" joined_len)
  if(NOT joined_len EQUAL total_bytes)
    message(FATAL_ERROR "embed_web：${path} 拼接后 ${joined_len} 字节，源文件 ${total_bytes} 字节")
  endif()
  file(SHA256 "${path}" expected_hash)
  string(SHA256 joined_hash "${joined}")
  if(NOT joined_hash STREQUAL expected_hash)
    message(FATAL_ERROR "embed_web：${path} 拼接结果 SHA256 ${joined_hash} 与源文件 ${expected_hash} 不一致")
  endif()
  if(max_chunk GREATER 4096)
    message(FATAL_ERROR "embed_web：${path} 存在 ${max_chunk} 字节的分块，超过 4 KB 上限")
  endif()
  set(${out_var} "${parts}" PARENT_SCOPE)
endfunction()

function(emit_hex_literal out_var path)
  file(READ "${path}" content HEX)
  string(LENGTH "${content}" total_hex)
  # 十六进制串每字节两字符，按 4096 字符切不会切开字节；
  # 偏移按十六进制字符算，与 SUBSTRING 同单位。
  set(offset 0)
  set(parts "")
  while(offset LESS total_hex)
    string(SUBSTRING "${content}" ${offset} 4096 chunk)
    string(APPEND parts "R\"DEVCARD(${chunk})DEVCARD\"\n")
    math(EXPR offset "${offset} + 4096")
  endwhile()
  set(${out_var} "${parts}" PARENT_SCOPE)
endfunction()

emit_text_literal(index_html "${INPUT_DIR}/index.html")
emit_text_literal(styles_css "${INPUT_DIR}/styles.css")
emit_text_literal(app_js "${INPUT_DIR}/app.js")
# 打印卡片的 logo 以十六进制内嵌，运行时由壳/服务端转回二进制提供 /logo.jpg，
# 打包时不必在可执行文件旁另放资源目录。
emit_hex_literal(logo_hex "${INPUT_DIR}/logo.jpg")

foreach(name index.html styles.css app.js logo.jpg)
  file(SHA256 "${INPUT_DIR}/${name}" hash)
  file(SIZE "${INPUT_DIR}/${name}" size)
  set(hash_${name} "${hash}")
  set(size_${name} "${size}")
endforeach()

file(WRITE "${OUTPUT}"
"// 由 cmake/embed_web.cmake 生成，不要手工编辑。\n"
"#include \"web_assets.h\"\n"
"\n"
"namespace devcard {\n"
"namespace {\n"
"const char kIndexHtml[] =\n${index_html};\n"
"const char kStylesCss[] =\n${styles_css};\n"
"const char kAppJs[] =\n${app_js};\n"
"const char kLogoJpgHex[] =\n${logo_hex};\n"
"}  // namespace\n"
"\n"
"const char* embedded_index_html() { return kIndexHtml; }\n"
"const char* embedded_styles_css() { return kStylesCss; }\n"
"const char* embedded_app_js() { return kAppJs; }\n"
"const char* embedded_logo_jpg_hex() { return kLogoJpgHex; }\n"
"\n"
"std::size_t embedded_index_html_size() { return sizeof(kIndexHtml) - 1; }\n"
"std::size_t embedded_styles_css_size() { return sizeof(kStylesCss) - 1; }\n"
"std::size_t embedded_app_js_size() { return sizeof(kAppJs) - 1; }\n"
"\n"
"// 期望值来自直读 web/ 源文件；运行时对这些字符串重新算 SHA256 对比。\n"
"const char* embedded_index_html_sha256() { return \"${hash_index.html}\"; }\n"
"const char* embedded_styles_css_sha256() { return \"${hash_styles.css}\"; }\n"
"const char* embedded_app_js_sha256() { return \"${hash_app.js}\"; }\n"
"const char* embedded_logo_jpg_sha256() { return \"${hash_logo.jpg}\"; }\n"
"\n"
"std::size_t embedded_source_size(EmbeddedAsset asset) {\n"
"  switch (asset) {\n"
"    case EmbeddedAsset::kIndexHtml: return ${size_index.html};\n"
"    case EmbeddedAsset::kStylesCss: return ${size_styles.css};\n"
"    case EmbeddedAsset::kAppJs: return ${size_app.js};\n"
"    case EmbeddedAsset::kLogoJpg: return ${size_logo.jpg};\n"
"  }\n"
"  return 0;\n"
"}\n"
"\n"
"const char* embedded_source_sha256(EmbeddedAsset asset) {\n"
"  switch (asset) {\n"
"    case EmbeddedAsset::kIndexHtml: return embedded_index_html_sha256();\n"
"    case EmbeddedAsset::kStylesCss: return embedded_styles_css_sha256();\n"
"    case EmbeddedAsset::kAppJs: return embedded_app_js_sha256();\n"
"    case EmbeddedAsset::kLogoJpg: return embedded_logo_jpg_sha256();\n"
"  }\n"
"  return \"\";\n"
"}\n"
"\n"
"}  // namespace devcard\n")
