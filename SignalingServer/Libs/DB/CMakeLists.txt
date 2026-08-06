# ============================================================
# db — 数据库模块（SQLite，vcpkg 管理）
# ============================================================

find_package(unofficial-sqlite3 CONFIG REQUIRED)

file(GLOB DB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(ds_db STATIC ${DB_SOURCES})

target_include_directories(ds_db PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ds_db PUBLIC
    ds_global
    unofficial::sqlite3::sqlite3
)
