add_library(devcard_core STATIC
    src/db.cpp
    src/migrations.cpp
    src/audit.cpp
    src/dao/device_dao.cpp
    src/dao/user_dao.cpp
    src/dao/setting_dao.cpp
    src/dao/audit_dao.cpp
    src/dao/backup_dao.cpp
    src/dao/instance_dao.cpp
    src/service/device_service.cpp
    src/service/user_service.cpp
    src/backup.cpp
    src/image_meta.cpp
    src/paths.cpp
    src/password.cpp
    src/settings.cpp
    src/sha256.cpp
    src/update_manifest.cpp
    src/update_package.cpp
    src/xlsx.cpp
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/argon2.c"
    "${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/core.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/encoding.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/ref.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/thread.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/blake2/blake2b.c"
    "${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c"
    "${DEVCARD_SQLITE_DIR}/sqlite3.c"
)

target_include_directories(devcard_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/third_party"
        "${CMAKE_SOURCE_DIR}/third_party/argon2/include"
        # db.h 是公开头，直接 #include <sqlite3.h>；两个平台都用仓库这份。
        "${DEVCARD_SQLITE_DIR}"
    PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/miniz"
        "${CMAKE_SOURCE_DIR}/third_party/argon2/src"
)

target_compile_definitions(devcard_core PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_DQS=0
)

# 核心版本号注入：core 层 update_manifest.cpp 的 current_product_version() 返回此值。
# 根 CMakeLists.txt 从 VERSION.txt 读 DEVCARD_VERSION_STRING。
if(DEFINED DEVCARD_VERSION_STRING)
    target_compile_definitions(devcard_core PRIVATE
        DEVCARD_VERSION="${DEVCARD_VERSION_STRING}"
    )
endif()

set_source_files_properties(
    "${DEVCARD_SQLITE_DIR}/sqlite3.c"
    "${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/argon2.c"
    "${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/core.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/encoding.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/ref.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/thread.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/blake2/blake2b.c"
    PROPERTIES COMPILE_OPTIONS "-w"
)

find_package(Threads REQUIRED)
target_link_libraries(devcard_core PUBLIC Threads::Threads)
# UTF-8 边界（路径 ↔ UTF-8、宽窄转换、控制台代码页）：PUBLIC 让 server/shell 直接用。
target_link_libraries(devcard_core PUBLIC win_encoding)

# 可选构建号注入（Windows UTC yyyyMMddHHmm）：客户端同版本重发时按 build 比较。
devcard_add_build_stamp(devcard_core)
