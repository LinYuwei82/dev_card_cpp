cmake_minimum_required(VERSION 3.16)
project(DevCard VERSION 1.1.3 LANGUAGES CXX)

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
