# 应用图标：随 bundle 复制到 Contents/Resources/，由 CFBundleIconFile 指向。
# 源文件用 .icns（已含 16–1024 全尺寸），不运行时生成，保证签名后图标哈希稳定。
set(DEVCARD_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/mac/DevCard.icns")
set_source_files_properties(${DEVCARD_APP_ICON} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")

add_executable(DevCard MACOSX_BUNDLE mac/main.mm ${DEVCARD_APP_ICON})
target_link_libraries(DevCard PRIVATE devcard_server "-framework Cocoa" "-framework WebKit"
    "-framework PDFKit" "-framework UniformTypeIdentifiers")

set_target_properties(DevCard PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "DevCard"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.anye.devcard.new"
    MACOSX_BUNDLE_ICON_FILE "DevCard.icns"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${DEVCARD_VERSION_STRING}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${DEVCARD_VERSION_STRING}"
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
