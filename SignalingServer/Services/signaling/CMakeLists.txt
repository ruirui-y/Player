# ============================================================
# signaling — 信令服务可执行文件
# ============================================================

file(GLOB SIG_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_executable(SignalingServer ${SIG_SOURCES})

target_include_directories(SignalingServer PRIVATE
    ${CMAKE_SOURCE_DIR}/Libs
)

target_link_libraries(SignalingServer PRIVATE
    ds_thread
    ds_db
    ds_global
    ds_http
    Threads::Threads
)

# 编译时自动复制 Config 到输出目录，VS 直接 F5 就能跑
add_custom_command(TARGET SignalingServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/Config"
    "$<TARGET_FILE_DIR:SignalingServer>/Config"
    COMMENT "Copying Config/ to output directory"
)
