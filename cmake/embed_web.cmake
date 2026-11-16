# 把 web/ 下的文件生成到 web_assets.cpp。
#
# 两条硬要求：
# 1) 单个字符串字面量不能太长。MSVC 的 C2026 上限约 16 KB，超了会**静默截断**：
#    页面照样能开，功能悄悄没了。所以按 4 KB 分块，用相邻字面量拼接（编译期合并）。
# 2) 必须有自检。分块写法本身不保证没截断，所以把「直读 web/ 源文件内容」的字节数与
#    SHA256 一起写进生成文件，运行时由 server 与 ctest 校验。
#    注意校验的是**源文件内容哈希**，不是整个 .cpp 的哈希——.cpp 里有生成路径等信息，
#    跨机器必然不同，那种哈希只会逼人把自检关掉。

# 说明：string(LENGTH) 返回字符还是字节，历史上换过说法（4.4.x 上实测按字节）。
# 生成器不押这个语义：长度一律由 file(READ … HEX) 的十六进制串换算（长度 / 2 即字节数），
# 切片偏移也按字节推进。HEX 串是纯 ASCII，两个单位不会分叉。
function(emit_text_literal out_var path)
  file(READ "${path}" content)
  file(READ "${path}" content_hex HEX)
  string(LENGTH "${content_hex}" total_hex)
  math(EXPR total_bytes "${total_hex} / 2")
  # 换行：CMake 的 file(READ) 会把 CRLF 折成 LF（HEX 读是原始字节，两者到这里就分叉），
  # 所以生成器对含 CR 的源文件**做不到按字节保真**。与其静默改写，不如直接失败。
  # 仓库里 web/ 另有 .gitattributes 定死 LF，两条结论一致：文本源必须是 LF。
  #
  # 这里不用正则：CMake 的正则引擎是回溯式的，`^([0-9a-f][0-9a-f])*0d` 这种带量词的写法
  # 在长串上会栈溢出，失败形态是「cmake 进程直接消失」（Windows 上 0xC00000FD / MSB8066），
  # 报错信息指不到现场。60,000 字符起就崩，app.js 的 hex 串是 112,402 字符。
  #
  # 做法：按 4096 个 hex 字符切片做线性扫描，窗口前移只在片内发生。
  #   - 切片长度必须是偶数：片起点因此恒为偶数绝对偏移，一个字节的两个 hex 字符不会被切到两片里，
  #     "0d" 也就不会跨片漏判。有人把下面的 cr_slice 改成奇数，这条保证就没了。
  #   - 片内每命中一次就把前缀切掉继续找，最坏退化被限制在 4096 字符以内；
  #     不对整串反复 SUBSTRING，避免对抗输入（如反复 20 d1）退化成平方级。
  #   - 必须判奇偶：`0x?0 0xd?` 拼出的 hex 里也会出现 "0d"，落在奇数位，
  #     那是两个字节各出一位，不是 CR。
  set(cr_slice 4096)
  string(LENGTH "${content_hex}" cr_hexlen)
  set(cr_base 0)
  while(cr_base LESS cr_hexlen)
    math(EXPR cr_width "${cr_hexlen} - ${cr_base}")
    if(cr_width GREATER cr_slice)
      set(cr_width ${cr_slice})
    endif()
    string(SUBSTRING "${content_hex}" ${cr_base} ${cr_width} cr_work)
    set(cr_drop 0)
    while(TRUE)
      string(FIND "${cr_work}" "0d" cr_rel)
      if(cr_rel EQUAL -1)
        break()
      endif()
      math(EXPR cr_abs "${cr_base} + ${cr_drop} + ${cr_rel}")
      math(EXPR cr_parity "${cr_abs} % 2")
      if(cr_parity EQUAL 0)
        math(EXPR cr_byte "${cr_abs} / 2")
        message(FATAL_ERROR "embed_web：${path} 含 CR（0x0D），首处位于第 ${cr_byte} 字节（0 起）。"
                            "文本源必须是 LF：CMake 读文件会把 CRLF 折成 LF，生成器无法按字节保真，"
                            "仓库里 web/ 由 .gitattributes 定死 LF")
      endif()
      # 奇数位命中：`0x?0 0xd?` 的两个 nibble 碰巧拼成 "0d"，跳过这一对继续。
      # 跳过 2 个字符是安全的：下一个可能的 CR 起点是 cr_rel+2（偶数位）；cr_rel+1 已被 'd' 占住，
      # 不可能再是 "0d" 的起点。
      math(EXPR cr_skip "${cr_rel} + 2")
      string(SUBSTRING "${cr_work}" ${cr_skip} -1 cr_work)
      math(EXPR cr_drop "${cr_drop} + ${cr_skip}")
    endwhile()
    math(EXPR cr_base "${cr_base} + ${cr_slice}")
  endwhile()
  set(offset 0)
  set(parts "")
  set(joined "")
  set(blocks 0)
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
    # 最后一块要按剩余字节数收口：SUBSTRING 越界会自动截短，但 take 不跟着改，
    # 就会把 offset 加过头，后面的「写出字节数 == 源文件字节数」断言反而误报。
    math(EXPR remain "${total_bytes} - ${offset}")
    if(take GREATER remain)
      set(take ${remain})
    endif()
    # 块首不能是续字节：这是「产物是合法 UTF-8」的结构保证，不靠事后全量解码。
    math(EXPR offset_hex "${offset} * 2")
    string(SUBSTRING "${content_hex}" ${offset_hex} 2 first_hex)
    math(EXPR first_byte "0x${first_hex}")
    if(offset GREATER 0 AND first_byte GREATER_EQUAL 128 AND first_byte LESS 192)
      message(FATAL_ERROR "embed_web：${path} 的块首 ${offset} 落在 UTF-8 续字节上")
    endif()
    if(take GREATER max_chunk)
      set(max_chunk ${take})
    endif()
    string(SUBSTRING "${content}" ${offset} ${take} chunk)
    # 换行不能以字面 LF 写进生成文件：Windows 上 CMake 以文本模式写文件，正文里的 LF
    # 会变成 CRLF，嵌入字节与源文件就不一致（运行期 SHA256 自检会失败，编译期看不出来）。
    # 切成相邻字面量：R"DEVCARD(段)DEVCARD" "\n" R"DEVCARD(段)DEVCARD"，编译期拼回原字节。
    string(REPLACE "\n" ")DEVCARD\" \"\\n\" R\"DEVCARD(" chunk_parts "${chunk}")
    string(APPEND parts "R\"DEVCARD(${chunk_parts})DEVCARD\"\n")
    string(APPEND joined "${chunk}")
    math(EXPR offset "${offset} + ${take}")
    math(EXPR blocks "${blocks} + 1")
  endwhile()
  if(total_bytes EQUAL 0)
    set(parts "R\"DEVCARD()DEVCARD\"\n")
  endif()

  # 构建期自检：生成器本身必须自洽，不能指望运行期测试兜底。
  # 1) 写出的字节数（按 take 累加，不用 string(LENGTH)）等于源文件字节数；
  # 2) 拼回来的 SHA256 等于源文件 SHA256；
  # 3) 每块不超过 4 KB（远离 MSVC C2026 的 16 KB 上限）；
  # 4) 块首不是续字节 → 每块都是合法 UTF-8 的片段，拼起来也合法。
  if(NOT offset EQUAL total_bytes)
    message(FATAL_ERROR "embed_web：${path} 写出 ${offset} 字节，源文件 ${total_bytes} 字节")
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
# 浏览器 favicon 同理：打包后没有磁盘 web/ 目录，必须随二进制内嵌。
emit_hex_literal(favicon_hex "${INPUT_DIR}/favicon.ico")

# 图标 / manifest 一类的静态资源：url|相对路径|MIME。
set(STATIC_ASSETS
  "/favicon-16x16.png|favicon-16x16.png|image/png"
  "/favicon-32x32.png|favicon-32x32.png|image/png"
  "/favicon-48x48.png|favicon-48x48.png|image/png"
  "/apple-touch-icon.png|apple-touch-icon.png|image/png"
  "/icons/pwa-192x192.png|icons/pwa-192x192.png|image/png"
  "/icons/pwa-512x512.png|icons/pwa-512x512.png|image/png"
  "/icons/pwa-maskable-192x192.png|icons/pwa-maskable-192x192.png|image/png"
  "/icons/pwa-maskable-512x512.png|icons/pwa-maskable-512x512.png|image/png"
  "/og-image.png|og-image.png|image/png"
  "/site.webmanifest|site.webmanifest|application/manifest+json")

set(static_decls "")
set(static_rows "")
set(static_index 0)
foreach(spec IN LISTS STATIC_ASSETS)
  string(REPLACE "|" ";" fields "${spec}")
  list(GET fields 0 url)
  list(GET fields 1 rel)
  list(GET fields 2 mime)
  set(var "static_${static_index}")
  emit_hex_literal(${var} "${INPUT_DIR}/${rel}")
  string(APPEND static_decls "const char* kStatic${static_index}Hex =\n${${var}};\n")
  string(APPEND static_rows "  {\"${url}\", \"${mime}\", kStatic${static_index}Hex},\n")
  math(EXPR static_index "${static_index} + 1")
endforeach()

foreach(name index.html styles.css app.js logo.jpg favicon.ico)
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
"const char kFaviconIcoHex[] =\n${favicon_hex};\n"
"}  // namespace\n"
"\n"
"const char* embedded_index_html() { return kIndexHtml; }\n"
"const char* embedded_styles_css() { return kStylesCss; }\n"
"const char* embedded_app_js() { return kAppJs; }\n"
"const char* embedded_logo_jpg_hex() { return kLogoJpgHex; }\n"
"const char* embedded_favicon_ico_hex() { return kFaviconIcoHex; }\n"
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
"const char* embedded_favicon_ico_sha256() { return \"${hash_favicon.ico}\"; }\n"
"\n"
"std::size_t embedded_source_size(EmbeddedAsset asset) {\n"
"  switch (asset) {\n"
"    case EmbeddedAsset::kIndexHtml: return ${size_index.html};\n"
"    case EmbeddedAsset::kStylesCss: return ${size_styles.css};\n"
"    case EmbeddedAsset::kAppJs: return ${size_app.js};\n"
"    case EmbeddedAsset::kLogoJpg: return ${size_logo.jpg};\n"
"    case EmbeddedAsset::kFaviconIco: return ${size_favicon.ico};\n"
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
"    case EmbeddedAsset::kFaviconIco: return embedded_favicon_ico_sha256();\n"
"  }\n"
"  return \"\";\n"
"}\n"
"\n"
"}  // namespace devcard\n")

file(APPEND "${OUTPUT}"
"namespace devcard {\n"
"namespace {\n"
"${static_decls}"
"}  // namespace\n"
"\n"
"const StaticAsset kStaticAssets[] = {\n"
"${static_rows}"
"};\n"
"\n"
"const StaticAsset* embedded_static_assets() { return kStaticAssets; }\n"
"std::size_t embedded_static_asset_count() {\n"
"  return sizeof(kStaticAssets) / sizeof(kStaticAssets[0]);\n"
"}\n"
"\n"
"}  // namespace devcard\n")
