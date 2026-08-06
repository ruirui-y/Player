# ============================================================
# thread — 线程池模块（Boost 版）
# ============================================================
# 基于 boost::asio::io_context 的线程池
# 每个 Worker 线程独立持有 SqlExec + RedisHelper（thread_local）
# ============================================================

# asio header-only，顶层已 find_package(Boost)

file(GLOB THREAD_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(ds_thread STATIC ${THREAD_SOURCES})

target_include_directories(ds_thread PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(ds_thread PUBLIC
    ds_global
    ds_db
    Threads::Threads
)