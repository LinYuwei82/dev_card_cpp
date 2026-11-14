add_executable(devcard_core_tests test_core.cpp)
target_link_libraries(devcard_core_tests PRIVATE devcard_core)
add_test(NAME devcard_core_tests COMMAND devcard_core_tests)

# 内嵌前端与 web/ 源文件的字节数/SHA256 一致性（抓 C2026 静默截断）。
add_executable(devcard_web_assets_tests test_web_assets.cpp)
target_link_libraries(devcard_web_assets_tests PRIVATE devcard_server)
add_test(NAME devcard_web_assets_tests COMMAND devcard_web_assets_tests)
