cmake_minimum_required(VERSION 3.16)

# ============================================================
# SignalingServer 顶层构建文件
# 公网信令服务器 — 设备注册、STUN、打洞信令、WebSocket 推送
# ============================================================

project(SignalingServer VERSION 1.0.0 LANGUAGES CXX)

# ---- C++ 标准 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 查找依赖包（来自 vcpkg）----
find_package(Boost REQUIRED COMPONENTS json log log_setup thread)
find_package(OpenSSL REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ---- 编译选项 ----
if(MSVC)
    add_compile_options(/W3 /utf-8 /wd4100)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_WIN32_WINNT=0x0A00)
    # Release 模式也生成 PDB（供 WPR 火焰图分析用）
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG:FULL /OPT:REF /OPT:ICF")
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations)
endif()

# ---- 包含子目录 ----
add_subdirectory(Libs)
add_subdirectory(Services)
# add_subdirectory(tests)