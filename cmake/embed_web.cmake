file(READ "${INPUT_DIR}/index.html" index_html)
file(READ "${INPUT_DIR}/styles.css" styles_css)
file(READ "${INPUT_DIR}/app.js" app_js)
# 打印卡片的 logo 以十六进制内嵌，运行时由壳/服务端转回二进制提供 /logo.jpg，
# 打包时不必在可执行文件旁另放资源目录。
file(READ "${INPUT_DIR}/logo.jpg" logo_hex HEX)
file(WRITE "${OUTPUT}"
"#include \"web_assets.h\"\n"
"namespace devcard {\n"
"const char* embedded_index_html() { return R\"DEVCARD(${index_html})DEVCARD\"; }\n"
"const char* embedded_styles_css() { return R\"DEVCARD(${styles_css})DEVCARD\"; }\n"
"const char* embedded_app_js() { return R\"DEVCARD(${app_js})DEVCARD\"; }\n"
"const char* embedded_logo_jpg_hex() { return R\"DEVCARD(${logo_hex})DEVCARD\"; }\n"
"}\n")
