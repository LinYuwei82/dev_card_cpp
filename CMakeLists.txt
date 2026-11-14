cmake_minimum_required(VERSION 3.16)
project(DevCard VERSION 1.1.3 LANGUAGES CXX)

# 新架构（core/server/shell）与现有 Qt 架构并行存在。
# 默认 OFF，保持 build.bat / Qt 的现有构建完全不变；新架构用
#   cmake -S . -B build-new -DDEVCARD_NEW_ARCH=ON
# 单独构建。M0 阶段不替换 Qt 构建入口。
option(DEVCARD_NEW_ARCH "构建新架构（core/server/shell），不编译 Qt 代码" OFF)

if(DEVCARD_NEW_ARCH)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if(MSVC)
        # 源码/资源/内嵌前端字符串统一按 UTF-8 解释（§10.1）。
        add_compile_options(/utf-8)
        # 静态 CRT：发布包不依赖 VC++ Redistributable。Windows 上需实测（§10.8.1）。
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
    endif()
    # 仅新架构有效：关掉后只编 core/server，不生成桌面壳。
    # 默认 ON，构建行为与之前完全一致；留作将来 Win7/8 独立分支换壳的入口。
    # 注意：这不解决 Win7/8 的真正卡点（WebView2 Runtime 109、toolset 降级），见 docs §13。
    option(DEVCARD_BUILD_SHELL "构建桌面壳（macOS WKWebView / Windows WebView2）" ON)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" DEVCARD_VERSION_STRING)
    string(STRIP "${DEVCARD_VERSION_STRING}" DEVCARD_VERSION_STRING)
    message(STATUS "DevCard 新架构版本：${DEVCARD_VERSION_STRING}")
    # SQLite 的唯一入口：两个平台都编译仓库自带的 amalgamation，不链接系统 libsqlite3。
    # 带版本号的目录只在这里出现一次；升级到 3.4x 时改这一行即可，
    # 理由不是「怕漏了只有某个平台报错」——统一之后两边一样会编不过——而是
    # 版本号本来就不该散落在多处，散一处就是一处将来的不一致。
    set(DEVCARD_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/sqlite-amalgamation-3460100")
    # Windows 版本资源与 macOS bundle 版本都从 VERSION.txt 取值。
    string(REPLACE "." "," DEVCARD_VERSION_COMMA "${DEVCARD_VERSION_STRING}")
    set(DEVCARD_VERSION_COMMA "${DEVCARD_VERSION_COMMA},0")
    set(DEVCARD_VERSION_RC_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/devcard_version.cmake")
    if(APPLE)
        enable_language(OBJCXX)
    endif()
    enable_language(C)
    add_subdirectory(core)
    add_subdirectory(server)
    if(DEVCARD_BUILD_SHELL)
        if(APPLE)
            add_subdirectory(shell)
        elseif(WIN32)
            add_subdirectory(shell/win)
        endif()
    endif()
    enable_testing()
    add_subdirectory(tests)
    return()
endif()


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

# 可选：第三方库根目录（用户可自行设置环境变量或 cmake -DTHIRDPARTY_DIR=xxx）
# 建议目录结构：
#   THIRDPARTY_DIR/
#     include/
#       hpdf.h
#       zip.h
#       xlsxwriter.h
#     lib/
#       libhpdf.lib / hpdf.lib
#       libzip.lib / zip.lib
#       xlsxwriter.lib
if(NOT DEFINED THIRDPARTY_DIR)
    set(THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/3rdparty" CACHE PATH "Third-party libraries root")
endif()

# 查找 Qt5 或 Qt6
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Sql Xml)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Sql Xml)

set(PROJECT_SOURCES
    src/main.cpp
    src/app_info.h
    src/db/database.h
    src/db/database.cpp
    src/db/initializer.h
    src/db/initializer.cpp
    src/excel/excel_handler.h
    src/excel/excel_handler.cpp
    src/printer/card_printer.h
    src/printer/card_printer.cpp
    src/ui/ui_login.h
    src/ui/ui_login.cpp
    src/ui/ui_main.h
    src/ui/ui_main.cpp
    src/ui/ui_user.h
    src/ui/ui_user.cpp
    src/ui/login_window.h
    src/ui/login_window.cpp
    src/ui/main_window.h
    src/ui/main_window.cpp
    src/ui/user_window.h
    src/ui/user_window.cpp
    resources/resources.qrc
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(DevCard
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
else()
    add_executable(DevCard
        ${PROJECT_SOURCES}
    )
endif()

target_include_directories(DevCard PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/excel
    ${CMAKE_SOURCE_DIR}/src/printer
    ${CMAKE_SOURCE_DIR}/src/ui
)

# 第三方库头文件目录
if(EXISTS "${THIRDPARTY_DIR}/include")
    target_include_directories(DevCard PRIVATE "${THIRDPARTY_DIR}/include")
endif()

target_link_libraries(DevCard PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Xml
)

# 第三方库链接（静态库优先）
set(_3rd_lib_names
    "hpdf;libhpdf"
    "zip;libzip"
    "xlsxwriter;libxlsxwriter"
)
foreach(pair ${_3rd_lib_names})
    list(GET pair 0 name1)
    list(GET pair 1 name2)
    find_library(_found_lib_${name1}
        NAMES ${name1} ${name2}
        PATHS "${THIRDPARTY_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(_found_lib_${name1})
        target_link_libraries(DevCard PRIVATE "${_found_lib_${name1}}")
    else()
        # 回退：假设系统路径可用
        target_link_libraries(DevCard PRIVATE ${name1})
    endif()
endforeach()

# Windows 特有：子系统为窗口程序、SQLite 插件
if(WIN32)
    set_target_properties(DevCard PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
    find_package(Qt${QT_VERSION_MAJOR} OPTIONAL_COMPONENTS QUIET SqlDriver)
endif()

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(DevCard)
endif()
