# 新架构的 Windows 版本资源：所有取值都来自 VERSION.txt。
#
# 用法：devcard_add_version_rc(<exe target>)
# - 任何平台上都会在构建目录生成 version_<target>.rc，便于核对内容；
# - 只有在 WIN32 上才把它编进目标。
function(devcard_add_version_rc target)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/version_${target}.rc")
    set(DEVCARD_RC_FILENAME "${target}.exe")
    configure_file("${DEVCARD_VERSION_RC_TEMPLATE}" "${output}" @ONLY)
    if(WIN32)
        target_sources(${target} PRIVATE "${output}")
    endif()
endfunction()

# 可选构建号注入：传入时把数值作为 DEVCARD_BUILD_STAMP 编译期宏打入 target。
# 客户端 core/include/devcard/update_manifest.h 的 compiled_build_number() 读此常量。
# 不传（DEVCARD_BUILD 为空）时客户端函数返回 0，回退到当前行为。
# 注意：只能传数值，不能用 static_cast 等表达式——CMake 的 target_compile_definitions
# 会把括号拆成多个 -D 项。类型转换在 C++ 侧用 static_cast<uint64_t> 完成。
function(devcard_add_build_stamp target)
    if(DEFINED DEVCARD_BUILD AND NOT DEVCARD_BUILD STREQUAL "")
        # 两个独立宏：
        #   DEVCARD_BUILD_STAMP=<number>       — 整数，core 里 static_cast<uint64_t> 用
        #   DEVCARD_BUILD_STAMP_STR="<number>" — 字符串，shell 里 "DEVCARD_BUILD_STAMP=" DEVCARD_BUILD_STAMP_STR 拼接用
        target_compile_definitions(${target} PRIVATE
            DEVCARD_BUILD_STAMP=${DEVCARD_BUILD}
            DEVCARD_BUILD_STAMP_STR="${DEVCARD_BUILD}")
        message(STATUS "DevCard 构建号注入（${target}）：DEVCARD_BUILD_STAMP=${DEVCARD_BUILD}")
    else()
        message(STATUS "DevCard 未注入构建号（${target}，DEVCARD_BUILD 为空），同版本不比 build")
    endif()
endfunction()
