file(READ "${INPUT}" content)
file(WRITE "${OUTPUT}"
"#include \"web_assets.h\"\n"
"namespace devcard {\n"
"const char* embedded_index_html() {\n"
"  return R\"DEVCARD(${content})DEVCARD\";\n"
"}\n"
"}\n")
