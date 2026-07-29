#ifndef WINDOWCAPTURE_H
#define WINDOWCAPTURE_H

#include "CaptureCommon.h"
#include "DcCapture.h"
#include "WgcCapture.h"

#include <cstdint>

class WindowCapture
{
public:
    WindowCapture();                                                    // 构造
    ~WindowCapture();                                                   // 析构

    // 初始化，传入 D3D11 设备、目标窗口、采集方法和选项
    // device 为 nullptr 时自动降级到 BitBlt 路线
    bool Init(ID3D11Device* device, HWND window, WindowCaptureMethod method,
              bool cursor, bool compatibility, bool client_area, bool force_sdr);

    void Shutdown();                                                    // 停止采集并释放资源

    // 每帧调用，delta_seconds 为距上次的秒数
    void Tick(float delta_seconds);

    // 获取当前帧
    bool GetFrame(CaptureFrame& out_frame);

    uint32_t Width() const;                                             // 帧宽度
    uint32_t Height() const;                                            // 帧高度
    WindowCaptureMethod Method() const;                                 // 当前采集方法
    bool IsHooked() const;                                              // 是否已成功挂钩窗口

    // 静态方法：根据窗口类名判断是否应使用 WGC
    static bool ShouldUseWgc(const char* class_name);

    // 静态方法：获取窗口类名
    static bool GetWindowClass(HWND window, char* class_name, size_t count);

private:
    WindowCaptureMethod ChooseMethod(const char* class_name);           // 自动选择采集方法
    void ForceReset();                                                  // 重置采集状态
    bool WindowNormal() const;                                          // 窗口是否正常（存在且非最小化）
    void InitDpiFunctions();                                            // 加载 DPI 感知函数指针

    // ---- 配置 ----
    ID3D11Device* device_{nullptr};                                     // D3D11 设备（外部拥有）
    HWND window_{nullptr};                                              // 目标窗口句柄
    WindowCaptureMethod method_{WindowCaptureMethod::Auto};             // 采集方法
    bool cursor_{false};                                                // 是否采集光标
    bool compatibility_{false};                                         // 是否使用兼容模式
    bool client_area_{false};                                           // WGC 路线：是否仅采集客户区
    bool force_sdr_{false};                                             // 是否强制 SDR

    // ---- 采集器实例 ----
    DcCapture dc_capture_;                                              // GDI BitBlt 采集器
    WgcCapture wgc_capture_;                                            // WGC 采集器

    // ---- 状态 ----
    bool hooked_{false};                                                // 是否已成功挂钩
    bool previously_failed_{false};                                     // WGC 是否失败过（避免反复重试）
    float resize_timer_{0.0f};                                          // 窗口尺寸变化检测计时器
    float check_window_timer_{0.0f};                                    // 窗口查找重试计时器
    float cursor_check_time_{0.0f};                                     // 光标显隐检测计时器
    RECT last_rect_{0, 0, 0, 0};                                        // 上次的窗口客户区矩形

    // ---- DPI 感知函数指针（从 User32.dll 动态加载） ----
    DPI_AWARENESS_CONTEXT(WINAPI* set_thread_dpi_awareness_context_)(DPI_AWARENESS_CONTEXT){nullptr};
    DPI_AWARENESS_CONTEXT(WINAPI* get_thread_dpi_awareness_context_)(){nullptr};
    DPI_AWARENESS_CONTEXT(WINAPI* get_window_dpi_awareness_context_)(HWND){nullptr};
};

#endif // WINDOWCAPTURE_H
