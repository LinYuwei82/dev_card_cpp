# 应用图标：随 bundle 复制到 Contents/Resources/，由 CFBundleIconFile 指向。
# 源文件用 .icns（已含 16–1024 全尺寸），不运行时生成，保证签名后图标哈希稳定。
set(DEVCARD_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/mac/DevCard.icns")
set_source_files_properties(${DEVCARD_APP_ICON} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")

# M1：shell_state 承载启动期共享设施，about_panel 承载「关于」面板与常驻单例。
# M2：download_handler 承载导航/下载/JS 面板，print_export 承载导出与打印通道。
# M3：bridge 承载 JS 消息分发，app_delegate 承载应用/窗口生命周期。
# M4：window_host 承载窗口/WebView 装配，print_selftest 承载打印自检。
add_executable(DevCard MACOSX_BUNDLE
    mac/main.mm
    mac/shell_state.mm
    mac/about_panel.mm
    mac/download_handler.mm
    mac/print_export.mm
    mac/bridge.mm
    mac/app_delegate.mm
    mac/window_host.mm
    mac/print_selftest.mm
    mac/updater.mm
    ${DEVCARD_APP_ICON})
target_include_directories(DevCard PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/mac"
    "${SPARKLE_ROOT}/Sparkle.framework/Headers")

# Sparkle 2 自更新框架：放在仓库 3rdparty/sparkle，随包分发。
# CMAKE_FRAMEWORK_PATH 让 find_library 能找到；
# -Wl,-rpath,@loader_path/../Frameworks 让可执行文件在 bundle 内搜 framework。
set(SPARKLE_ROOT "${PROJECT_SOURCE_DIR}/3rdparty/sparkle")
find_library(SPARKLE Sparkle PATHS "${SPARKLE_ROOT}" REQUIRED)
target_link_libraries(DevCard PRIVATE devcard_server
    "-framework Cocoa" "-framework WebKit"
    "-framework PDFKit" "-framework UniformTypeIdentifiers"
    "${SPARKLE}")
set_target_properties(DevCard PROPERTIES
    BUILD_RPATH "@loader_path/../Frameworks"
    INSTALL_RPATH "@loader_path/../Frameworks"
)

# Sparkle.framework 必须保留 symlink（Versions/Current -> B，顶层 Sparkle/Headers
# 等均是指向 Versions/Current 的链接）。CMake 的 MACOSX_PACKAGE_LOCATION 拷贝会
# 解引用 symlink，使 framework 结构非法、codesign 报 "bundle format is ambiguous"，
# 因此改用 ditto（保留符号链接与资源分叉）在 POST_BUILD 阶段嵌入。
set(DEVCARD_SPARKLE_FRAMEWORK
    "${PROJECT_SOURCE_DIR}/3rdparty/sparkle/Sparkle.framework")
add_custom_command(
    TARGET DevCard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_DIR:DevCard>/Contents/Frameworks/Sparkle.framework"
    COMMAND ditto "${DEVCARD_SPARKLE_FRAMEWORK}"
            "$<TARGET_BUNDLE_DIR:DevCard>/Contents/Frameworks/Sparkle.framework"
    # DevCard 不开启 App Sandbox，官方文档明确：非沙盒应用应移除 Sparkle 的两个
    # XPC 服务（Installer.xpc / Downloader.xpc），它们只在沙盒环境下配合
    # SUEnableInstallerLauncherService / SUEnableDownloaderService 才生效。
    # 保留它们会让 Sparkle 探测 <bundle-id>-spks 失败并卡在下载阶段。
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_DIR:DevCard>/Contents/Frameworks/Sparkle.framework/Versions/B/XPCServices"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_DIR:DevCard>/Contents/Frameworks/Sparkle.framework/XPCServices"
    COMMENT "嵌入 Sparkle.framework（ditto，保留 symlink；非沙盒移除 XPC 服务）"
    VERBATIM
)

# Sparkle 2：EdDSA 公钥固定（除非换密钥对），feed URL 发布时才填。
# 开发期两键空时 Info.plist 里不写 Sparkle 键，Sparkle 不启动。
if(NOT DEVCARD_SPARKLE_PUBKEY)
    set(DEVCARD_SPARKLE_PUBKEY "qYiCyNabGf5xZYo8rWFXRoRXkZA6sgfwcyGZ0JNGaXA=")
endif()
if(NOT DEVCARD_SPARKLE_FEED_URL)
    set(DEVCARD_SPARKLE_FEED_URL "")
endif()

# Info.plist 用 Python 生成（tools/release/gen_info_plist.py，零依赖）。
# configure 阶段先生成一份（CMAKE 要求 MACOSX_BUNDLE_INFO_PLIST 文件必须已存在），
# 同时在 POST_BUILD 里保留（每次 cmake build 都会重新生成，确保 Sparkle 键更新）。
# 脚本内容不变时跳过写盘：否则该文件 mtime 会越过本次 configure 生成的
# build.ninja，ninja 反复判定需要重新 configure 并以 still dirty 失败。
set(DEVCARD_INFO_PLIST_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/DevCard.app/Contents/Info.plist")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/DevCard.app/Contents")

execute_process(
    COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tools/release/gen_info_plist.py"
            --version "${DEVCARD_VERSION_STRING}"
            --build "${DEVCARD_BUILD}"
            --bundle-id "com.anye.devcard.new"
            --name "DevCard"
            --feed-url "${DEVCARD_SPARKLE_FEED_URL}"
            --pubkey "${DEVCARD_SPARKLE_PUBKEY}"
            --output "${DEVCARD_INFO_PLIST_PATH}"
    RESULT_VARIABLE DEVCARD_PLIST_GEN_RESULT
    OUTPUT_QUIET
)
if(NOT DEVCARD_PLIST_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "gen_info_plist.py failed (rc=${DEVCARD_PLIST_GEN_RESULT})")
endif()

add_custom_command(
    TARGET DevCard POST_BUILD
    COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tools/release/gen_info_plist.py"
            --version "${DEVCARD_VERSION_STRING}"
            --build "${DEVCARD_BUILD}"
            --bundle-id "com.anye.devcard.new"
            --name "DevCard"
            --feed-url "${DEVCARD_SPARKLE_FEED_URL}"
            --pubkey "${DEVCARD_SPARKLE_PUBKEY}"
            --output "${DEVCARD_INFO_PLIST_PATH}"
    COMMENT "重新生成 Info.plist"
    VERBATIM
)

set_target_properties(DevCard PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${DEVCARD_INFO_PLIST_PATH}"
    MACOSX_BUNDLE_BUNDLE_NAME "DevCard"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.anye.devcard.new"
    MACOSX_BUNDLE_ICON_FILE "DevCard.icns"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${DEVCARD_VERSION_STRING}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${DEVCARD_BUILD}"
)

# 子集字体随包分发到 Contents/Resources/fonts：server 的 default_font_dir()
# 按可执行文件位置往上找 Resources/fonts，界面 @font-face 与打印 PDF 共用这一份。
# 不进可执行文件：4.5 MB 的字体转成 C++ 字面量要翻倍，还会撞上 MSVC 的字符串长度限制。
set(DEVCARD_FONT_FILES
    "${PROJECT_SOURCE_DIR}/web/fonts/NotoSansSC-Regular.subset.ttf"
    "${PROJECT_SOURCE_DIR}/web/fonts/NotoSansSC-Bold.subset.ttf"
)

# 单独一个目标：POST_BUILD 只在 DevCard 真正重链时触发，
# 只换字体不换代码时不会复制，包里的字体就会悄悄过期。
# 路径不用生成器表达式：$<TARGET_BUNDLE_CONTENT_DIR:DevCard> 会让两者互相依赖成环。
set(DEVCARD_BUNDLE_FONT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/DevCard.app/Contents/Resources/fonts")
add_custom_target(devcard_bundle_fonts ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEVCARD_BUNDLE_FONT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${DEVCARD_FONT_FILES}
        "${DEVCARD_BUNDLE_FONT_DIR}"
    COMMENT "复制子集字体到 DevCard.app/Contents/Resources/fonts"
)
add_dependencies(DevCard devcard_bundle_fonts)
