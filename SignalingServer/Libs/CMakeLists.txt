# ============================================================
# libs — 公共库顶层
# 编译所有子模块为静态库，供各服务链接
# ============================================================

add_subdirectory(Global)
add_subdirectory(Thread)
add_subdirectory(DB)
add_subdirectory(HttpServer)