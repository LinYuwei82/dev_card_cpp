add_executable(devcard_core_tests test_core.cpp)
target_link_libraries(devcard_core_tests PRIVATE devcard_core)
add_test(NAME devcard_core_tests COMMAND devcard_core_tests)

# 内嵌前端与 web/ 源文件的字节数/SHA256 一致性（抓 C2026 静默截断）；
# 顺带复核字体清单里的字节数与 SHA256（构建期从 web/fonts/ 生成）。
add_executable(devcard_web_assets_tests test_web_assets.cpp)
target_link_libraries(devcard_web_assets_tests PRIVATE devcard_server)
target_compile_definitions(devcard_web_assets_tests PRIVATE
    DEVCARD_WEB_FONTS_DIR="${CMAKE_SOURCE_DIR}/web/fonts"
    DEVCARD_SERVER_SOURCE="${CMAKE_SOURCE_DIR}/server/src/server.cpp"
    DEVCARD_CORE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME devcard_web_assets_tests COMMAND devcard_web_assets_tests)

# 新架构不得引用旧 Qt 线的 OPPOSans：查源码、可执行文件字符串、随包 fonts 目录。
add_test(NAME devcard_no_opposans_tests
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DBINARY=$<TARGET_FILE:devcard-server>
        -DFONTS_DIR=${CMAKE_SOURCE_DIR}/web/fonts
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_opposans.cmake)

# 生成器本身的验收：跨字符切口、尺寸、换行保真、CR 政策、块数。
add_executable(devcard_embed_web_tests test_embed_web.cpp)
target_link_libraries(devcard_embed_web_tests PRIVATE devcard_core)
target_compile_definitions(devcard_embed_web_tests PRIVATE
    DEVCARD_CMAKE_COMMAND="${CMAKE_COMMAND}"
    DEVCARD_EMBED_SCRIPT="${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake")
add_test(NAME devcard_embed_web_tests COMMAND devcard_embed_web_tests)

# §10.7 性能基准与 M7 并发压测：手动入口，不注册 ctest（耗时且数字随机器变）。
# 退出码语义：0 达标 / 1 未达标 / 3 环境不具备。
add_executable(devcard_bench_core ${CMAKE_SOURCE_DIR}/tools/perf/bench_core.cpp)
target_link_libraries(devcard_bench_core PRIVATE devcard_core)

add_executable(devcard_bench_http ${CMAKE_SOURCE_DIR}/tools/perf/load_http.cpp)
target_link_libraries(devcard_bench_http PRIVATE devcard_server)
target_compile_definitions(devcard_bench_http PRIVATE
    DEVCARD_WEB_FONTS_DIR="${CMAKE_SOURCE_DIR}/web/fonts")
