add_library(devcard_core STATIC
    src/db.cpp
    src/migrations.cpp
    src/dao/device_dao.cpp
    src/service/device_service.cpp
    src/sha256.cpp
    "${CMAKE_SOURCE_DIR}/3rdparty/sqlite-amalgamation-3460100/sqlite3.c"
)

target_include_directories(devcard_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/third_party"
)

target_compile_definitions(devcard_core PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_DQS=0
)

set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/3rdparty/sqlite-amalgamation-3460100/sqlite3.c"
    PROPERTIES COMPILE_OPTIONS "-w"
)

find_package(Threads REQUIRED)
target_link_libraries(devcard_core PUBLIC Threads::Threads)
