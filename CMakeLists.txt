cmake_minimum_required(VERSION 3.16)

# 版本唯一来源：VERSION.txt。
# project() 的 VERSION 需要字面量，所以在 project() 之前先把文件读进来。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" DEVCARD_VERSION_STRING)
string(STRIP "${DEVCARD_VERSION_STRING}" DEVCARD_VERSION_STRING)
project(DevCard VERSION ${DEVCARD_VERSION_STRING} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    # 源码/资源/内嵌前端字符串统一按 UTF-8 解释（§10.1）。
    add_compile_options(/utf-8)
    # rc.exe 默认按系统 ANSI 码页解析 .rc，中文版本资源必须显式 /utf-8。
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /utf-8")
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
# 可选的构建号（发布 tag v<semver>+<seq> 的序号）。传入后编译期注入 DEVCARD_BUILD，
# 客户端同版本重发时按 build 比较（远端 build 大于本地才提示更新）；不传则
# compiled_build_number() 返回 0，客户端回退到当前「同版本不比 build」的逻辑。
# 正式发布由 tools/release/release_tag.py 取号（build = seq），再传入这里。
set(DEVCARD_DEFAULT_BUILD "999999999")
set(DEVCARD_BUILD "" CACHE STRING "构建号（发布 tag 的 seq；开发态留空回落到 999999999）")
option(DEVCARD_RELEASE_BUILD "发布/出包构建：DEVCARD_BUILD 必须显式提供（发布 tag 的 seq）" OFF)
if(DEVCARD_RELEASE_BUILD)
  if(DEVCARD_BUILD STREQUAL "")
    message(FATAL_ERROR "发布/出包构建必须显式指定 -DDEVCARD_BUILD=<seq>（发布 tag v<semver>+<seq> 的序号），例如：\n  cmake -B build/release -DDEVCARD_RELEASE_BUILD=ON -DDEVCARD_BUILD=2 ...")
  endif()
  if(NOT DEVCARD_BUILD MATCHES "^[0-9]+$")
    message(FATAL_ERROR "DEVCARD_BUILD 必须是纯数字：${DEVCARD_BUILD}")
  endif()
  if(DEVCARD_BUILD STREQUAL DEVCARD_DEFAULT_BUILD)
    message(FATAL_ERROR "发布/出包构建不能使用开发默认构建号 ${DEVCARD_DEFAULT_BUILD}")
  endif()
  unset(DEVCARD_RELEASE_BUILD CACHE)
elseif(DEVCARD_BUILD STREQUAL "")
  set(DEVCARD_BUILD "${DEVCARD_DEFAULT_BUILD}" CACHE STRING "" FORCE)
endif()
if(DEVCARD_BUILD STREQUAL DEVCARD_DEFAULT_BUILD)
  message(WARNING "构建号 ${DEVCARD_BUILD} 是开发占位值，此产物不可用于发布；\n  出包请用 tools/package/package_macos.sh / package_windows.ps1（发布号来自 tag 序号）。")
endif()
message(STATUS "DevCard 版本：${DEVCARD_VERSION_STRING}")
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
add_subdirectory(modules/win_encoding)
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
