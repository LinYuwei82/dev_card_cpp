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
  file(READ "${path}" content_hex HEX)   # 只用来判断分块边界是否落在 UTF-8 多字节中间
  string(LENGTH "${content}" total)
  string(LENGTH "${content_hex}" total_hex)
  set(offset 0)
  set(parts "")
  while(offset LESS total)
    math(EXPR take "4096")
    # 若切点后一个字节是 UTF-8 续字节（0x80–0xBF），说明切在了汉字中间：
    # 一个字节一个字节往回退。按字节切开会生成非 UTF-8 源文件，MSVC /utf-8 会报错。
    while(true)
      math(EXPR cut "${offset} + ${take}")
      if(cut GREATER_EQUAL total)
        break()
      endif()
      # content_hex 每个字节占两个字符，索引要乘 2
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
    string(APPEND parts "R\"DEVCARD(${chunk})DEVCARD\"\n")
    math(EXPR offset "${offset} + ${take}")
  endwhile()
  set(${out_var} "${parts}" PARENT_SCOPE)
endfunction()

function(emit_hex_literal out_var path)
  file(READ "${path}" content HEX)
  string(LENGTH "${content}" total)
  set(offset 0)
  set(parts "")
  while(offset LESS total)
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
