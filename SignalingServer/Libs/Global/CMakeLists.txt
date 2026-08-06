# ============================================================
# global — 全局配置与基础设施模块（含日志）
# ============================================================

file(GLOB GLOBAL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(ds_global STATIC ${GLOBAL_SOURCES})

target_include_directories(ds_global PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(ds_global PUBLIC
    Boost::json
    Boost::log
    Boost::log_setup
    Boost::thread
    fmt::fmt
)