#ifndef CAPTURECOMMON_H
#define CAPTURECOMMON_H

#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

// ========== 常量定义 ==========

constexpr float RESET_INTERVAL_SEC = 3.0f;                              // DXGI/WGC 失败后重试间隔（秒）
constexpr float WC_CHECK_TIMER = 1.0f;                                  // 窗口采集：找不到窗口时的重检间隔
constexpr float RESIZE_CHECK_TIME = 0.2f;                               // 窗口采集：检测窗口尺寸变化的间隔
constexpr float CURSOR_CHECK_TIME = 0.2f;                               // 窗口采集：检测光标显隐的间隔

// ========== 枚举 ==========

enum class DisplayCaptureMethod                                         // 显示器采集方法
{
    Auto,                                                               // 自动选择
    Dxgi,                                                               // DXGI Desktop Duplication
    Wgc,                                                                // Windows Graphics Capture
    Gdi,                                                                // 无 D3D11 设备时的回退路线
};

enum class WindowCaptureMethod                                          // 窗口采集方法
{
    Auto,                                                               // 自动选择
    BitBlt,                                                             // GDI BitBlt
    Wgc,                                                                // Windows Graphics Capture
};

// ========== 帧输出结构 ==========

struct CaptureFrame                                                     // 采集器统一输出的一帧
{
    ID3D11Texture2D* gpu_texture{nullptr};                              // GPU 纹理（DXGI/WGC 路线），不拥有所有权
    const uint8_t* cpu_data{nullptr};                                   // CPU 内存数据（GDI 路线），指向内部缓冲
    uint32_t cpu_stride{0};                                             // CPU 数据每行字节数
    uint32_t width{0};                                                  // 帧宽度
    uint32_t height{0};                                                 // 帧高度
    int rotation{0};                                                    // 旋转角度（0/90/180/270），仅 DXGI 路线有效

    bool IsValid() const { return gpu_texture != nullptr || cpu_data != nullptr; }
    bool IsGpu() const { return gpu_texture != nullptr; }
};

// ========== 光标信息结构 ==========

struct CursorInfo                                                       // 光标渲染所需信息
{
    const uint8_t* bitmap{nullptr};                                     // BGRA 位图数据，指向内部缓冲
    uint32_t width{0};                                                  // 光标宽度
    uint32_t height{0};                                                 // 光标高度
    long x_hotspot{0};                                                  // 热点 X 偏移
    long y_hotspot{0};                                                  // 热点 Y 偏移
    long screen_x{0};                                                   // 光标在屏幕中的 X 坐标
    long screen_y{0};                                                   // 光标在屏幕中的 Y 坐标
    bool visible{false};                                                // 是否可见
    bool monochrome{false};                                             // 是否为单色光标
};

// ========== 显示器信息结构 ==========

struct MonitorInfo                                                      // 显示器枚举信息
{
    char device_id[128]{0};                                             // 设备 ID（如 \\?\DISPLAY#...）
    char alt_id[128]{0};                                                // 备用 ID（如 \\.\DISPLAY1）
    char name[128]{0};                                                  // 友好名称（如 "DELL U2720Q"）
    RECT rect{0, 0, 0, 0};                                              // 显示器矩形
    HMONITOR handle{nullptr};                                           // 显示器句柄
};

// ========== 辅助函数声明 ==========

bool FindMonitorById(const char* monitor_id, MonitorInfo& out_info);    // 按设备 ID 查找显示器
bool GetMonitorName(HMONITOR handle, char* name, size_t count);         // 获取显示器友好名称

#endif // CAPTURECOMMON_H
