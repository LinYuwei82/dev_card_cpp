set(WEB_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(WEB_DIR "${CMAKE_SOURCE_DIR}/web")
file(GLOB WEB_JS_FILES "${WEB_DIR}/js/*.js")
add_custom_command(
    OUTPUT "${WEB_ASSETS_CPP}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT_DIR=${WEB_DIR}
            -DOUTPUT=${WEB_ASSETS_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake
    DEPENDS "${WEB_DIR}/index.html"
            "${WEB_DIR}/styles.css"
            "${WEB_DIR}/app.js"
            ${WEB_JS_FILES}
            "${WEB_DIR}/logo.jpg"
            "${WEB_DIR}/favicon.ico"
            "${WEB_DIR}/favicon-16x16.png"
            "${WEB_DIR}/favicon-32x32.png"
            "${WEB_DIR}/favicon-48x48.png"
            "${WEB_DIR}/apple-touch-icon.png"
            "${WEB_DIR}/icons/pwa-192x192.png"
            "${WEB_DIR}/icons/pwa-512x512.png"
            "${WEB_DIR}/icons/pwa-maskable-192x192.png"
            "${WEB_DIR}/icons/pwa-maskable-512x512.png"
            "${WEB_DIR}/og-image.png"
            "${WEB_DIR}/site.webmanifest"
            "${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake"
    VERBATIM
)

set(FONT_MANIFEST_H "${CMAKE_CURRENT_BINARY_DIR}/generated/font_manifest.h")
add_custom_command(
    OUTPUT "${FONT_MANIFEST_H}"
    COMMAND ${CMAKE_COMMAND}
            -DFONT_DIR=${WEB_DIR}/fonts
            -DOUTPUT=${FONT_MANIFEST_H}
            -P ${CMAKE_SOURCE_DIR}/cmake/font_manifest.cmake
    DEPENDS "${WEB_DIR}/fonts/NotoSansSC-Regular.subset.ttf"
            "${WEB_DIR}/fonts/NotoSansSC-Bold.subset.ttf"
            "${CMAKE_SOURCE_DIR}/cmake/font_manifest.cmake"
    VERBATIM
)

add_library(devcard_server STATIC
    src/server.cpp
    src/apply_update.cpp
    src/platform_paths.cpp
    src/config.cpp
    src/instance_lock.cpp
    src/heartbeat.cpp
    src/font_coverage.cpp
    src/web_assets_check.cpp
    "${WEB_ASSETS_CPP}"
    "${FONT_MANIFEST_H}"
)
target_include_directories(devcard_server PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_SOURCE_DIR}/third_party/httplib"
)
target_compile_definitions(devcard_server PRIVATE
    DEVCARD_VERSION="${DEVCARD_VERSION_STRING}"
    # accept 队列深度。httplib 默认 5（third_party/httplib/httplib.h:141），
    # 20 并发建连会被拒；只从构建侧抬高，不改内嵌依赖本体。
    CPPHTTPLIB_LISTEN_BACKLOG=128
)
target_link_libraries(devcard_server PUBLIC devcard_core)
if(WIN32)
    # ws2_32: sockets（httplib / inet_ntop）；iphlpapi: 枚举本机私有 IPv4；
    # version: 读 PE 版本资源；shell32: CommandLineToArgvW（重启时沿用原参数）。
    target_link_libraries(devcard_server PUBLIC ws2_32 iphlpapi version shell32)
endif()

# 可选构建号注入（两个 target 都要打：devcard_server 是库、devcard-server 是可执行）。
devcard_add_build_stamp(devcard_server)

add_executable(devcard-server src/main.cpp)
devcard_add_version_rc(devcard-server)
devcard_add_build_stamp(devcard-server)
target_compile_definitions(devcard-server PRIVATE
    DEVCARD_VERSION="${DEVCARD_VERSION_STRING}"
)
target_link_libraries(devcard-server PRIVATE devcard_server)

# 字体必须跟着每个可执行文件走：只管 DevCard 的话，server 单独分发到别的目录时
# /fonts/ 会 404，导出在运行期才中止。两份字体是导出前门禁，缺一颗都不行。
set(DEVCARD_SERVER_FONTS
    "${PROJECT_SOURCE_DIR}/web/fonts/NotoSansSC-Regular.subset.ttf"
    "${PROJECT_SOURCE_DIR}/web/fonts/NotoSansSC-Bold.subset.ttf")
add_custom_command(TARGET devcard-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:devcard-server>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${DEVCARD_SERVER_FONTS}
        "$<TARGET_FILE_DIR:devcard-server>/fonts"
    COMMENT "复制子集字体到 devcard-server 输出目录 fonts/"
)
