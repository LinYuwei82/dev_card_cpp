add_library(devcard_core STATIC
    src/db.cpp
    src/migrations.cpp
    src/dao/device_dao.cpp
    src/dao/user_dao.cpp
    src/service/device_service.cpp
    src/service/user_service.cpp
    src/backup.cpp
    src/legacy.cpp
    src/image_meta.cpp
    src/paths.cpp
    src/password.cpp
    src/settings.cpp
    src/sha256.cpp
    src/update_package.cpp
    src/xlsx.cpp
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/argon2.c"
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

set_source_files_properties(
    "${DEVCARD_SQLITE_DIR}/sqlite3.c"
    "${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/argon2.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/core.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/encoding.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/ref.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/thread.c"
    "${CMAKE_SOURCE_DIR}/third_party/argon2/src/blake2/blake2b.c"
    PROPERTIES COMPILE_OPTIONS "-w"
)

find_package(Threads REQUIRED)
target_link_libraries(devcard_core PUBLIC Threads::Threads)
