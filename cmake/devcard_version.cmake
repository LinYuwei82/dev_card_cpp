# 新架构的 Windows 版本资源：所有取值都来自 VERSION.txt。
# 旧 Qt 线继续用手写的 DevCard_resource.rc（已冻结在 v1.1.3）。
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
