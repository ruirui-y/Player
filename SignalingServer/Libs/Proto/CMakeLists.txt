# ============================================================
# proto — Protobuf 协议定义
# ============================================================

# 跨平台：先试 config 模式（Windows vcpkg），再试 module 模式（Linux apt）
find_package(Protobuf QUIET CONFIG)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()

file(GLOB PROTO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.proto")

protobuf_generate(
    PROTOS ${PROTO_FILES}
    OUT_VAR PROTO_SRCS
)

add_library(ds_proto STATIC ${PROTO_SRCS})

target_include_directories(ds_proto PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries(ds_proto PUBLIC protobuf::libprotobuf)