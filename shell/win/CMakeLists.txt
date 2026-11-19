# Windows 桌面壳（M5 第二段）。只在 WIN32 下由根 CMakeLists 调用。
#
# WebView2 SDK 的二进制不入库：先运行 fetch-webview2.ps1 下载并按 SHA256 校验。
# 未下载时这里直接报错并给出命令，不产出一个「能 configure 但编不了」的构建。

if(NOT DEFINED DEVCARD_WEBVIEW2_DIR OR DEVCARD_WEBVIEW2_DIR STREQUAL "")
    set(DEVCARD_WEBVIEW2_DIR "${PROJECT_SOURCE_DIR}/third_party/webview2")
endif()

set(DEVCARD_WEBVIEW2_INCLUDE "${DEVCARD_WEBVIEW2_DIR}/build/native/include")
set(DEVCARD_WEBVIEW2_LIB
    "${DEVCARD_WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")

if(NOT EXISTS "${DEVCARD_WEBVIEW2_INCLUDE}/WebView2.h"
   OR NOT EXISTS "${DEVCARD_WEBVIEW2_LIB}")
    message(FATAL_ERROR
        "缺少 WebView2 SDK（${DEVCARD_WEBVIEW2_DIR}）。\n"
        "先执行：powershell -ExecutionPolicy Bypass -File "
        "\"${CMAKE_CURRENT_SOURCE_DIR}/fetch-webview2.ps1\"\n"
        "无网环境：给该脚本传 -LocalPackage <nupkg 路径>。")
endif()

add_executable(DevCard WIN32 main.cpp app.manifest app.rc)
devcard_add_version_rc(DevCard)
devcard_add_build_stamp(DevCard)

# app.rc 里的 IDI_ICON1 引用同目录的 DevCard.ico；把当前目录加入资源编译
# 搜索路径，避免从别的构建目录调用时找不到图标。
target_include_directories(DevCard PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(DevCard PRIVATE UNICODE _UNICODE)
target_include_directories(DevCard PRIVATE
    "${DEVCARD_WEBVIEW2_INCLUDE}"
    "${PROJECT_SOURCE_DIR}/third_party/nlohmann"
)

# WebView2LoaderStatic 自身要求 version.lib 与 shlwapi.lib；
# 其余是 ShellExecute / 文件对话框 / 下载目录 / 打印机枚举所需。
target_link_libraries(DevCard PRIVATE
    devcard_server
    "${DEVCARD_WEBVIEW2_LIB}"
    ole32
    oleaut32
    uuid
    shell32
    shlwapi
    version
    user32
    gdi32
    advapi32
    winspool
)

# 子集字体放到 exe 旁的 fonts\：server 的 default_font_dir() 在 Windows 上
# 先找 <exe>\fonts。绿色版打包时把整个 fonts 目录一起带走即可。
set(DEVCARD_FONT_FILES
    "${PROJECT_SOURCE_DIR}/web/fonts/NotoSansSC-Regular.subset.ttf"
    "${PROJECT_SOURCE_DIR}/web/fonts/NotoSansSC-Bold.subset.ttf"
)
set_source_files_properties(${DEVCARD_FONT_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(DevCard PRIVATE ${DEVCARD_FONT_FILES})

# 必须用 $<TARGET_FILE_DIR:DevCard>：MSVC 是多配置生成器，exe 在 Debug\ 或 Release\ 下，
# 而 ${CMAKE_CURRENT_BINARY_DIR}/fonts 会落错一层。POST_BUILD 里用生成器表达式不会成环。
add_custom_command(TARGET DevCard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DevCard>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${DEVCARD_FONT_FILES}
        "$<TARGET_FILE_DIR:DevCard>/fonts"
    COMMENT "复制子集字体到输出目录 fonts\\"
)
