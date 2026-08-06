# ============================================================
# httpserver — HTTP Server & Client 模块
# ============================================================
# 基于 httplib 单头文件库的阻塞 HTTP Server 和 Client 封装
# ============================================================

file(GLOB HTTP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(ds_http STATIC ${HTTP_SOURCES})

target_include_directories(ds_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(ds_http PUBLIC
    ds_global
)