file(READ "${INPUT_DIR}/index.html" index_html)
file(READ "${INPUT_DIR}/styles.css" styles_css)
file(READ "${INPUT_DIR}/app.js" app_js)
file(WRITE "${OUTPUT}"
"#include \"web_assets.h\"\n"
"namespace devcard {\n"
"const char* embedded_index_html() { return R\"DEVCARD(${index_html})DEVCARD\"; }\n"
"const char* embedded_styles_css() { return R\"DEVCARD(${styles_css})DEVCARD\"; }\n"
"const char* embedded_app_js() { return R\"DEVCARD(${app_js})DEVCARD\"; }\n"
"}\n")
