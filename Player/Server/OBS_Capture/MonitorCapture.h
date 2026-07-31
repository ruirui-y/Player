#ifndef MONITORCAPTURE_H
#define MONITORCAPTURE_H

#include "CaptureCommon.h"
#include "CursorCapture.h"
#include "DcCapture.h"
#include "DxgiDuplicator.h"
#include "WgcCapture.h"

#include <vector>

class MonitorCapture
{
public:
    MonitorCapture();                                                   // 构造
    ~MonitorCapture();                                                  // 析构

    // 初始化，传入 D3D11 设备、显示器 ID、采集方法和选项
    // device 为 nullptr 时自动降级到 GDI 路线
    bool Init(ID3D11Device* device, const char* monitor_id,
              DisplayCaptureMethod method, bool capture_cursor, bool force_sdr);

    void Shutdown();                                                    // 停止采集并释放资源

    // 每帧调用，delta_seconds 为距上次的秒数
    void Tick(float delta_seconds);

    // 获取当前帧
    bool GetFrame(CaptureFrame& out_frame);

    // 获取光标信息（仅 DXGI 路线需要外部叠加光标，WGC 和 GDI 内部已处理）
    bool GetCursorInfo(CursorInfo& out_info);

    uint32_t Width() const;                                             // 帧宽度
    uint32_t Height() const;                                            // 帧高度
    DisplayCaptureMethod Method() const;                                // 当前采集方法
    bool IsActive() const;                                              // 是否正在采集
    int MonitorX() const { return monitor_x_; }                        // 显示器屏幕 X 偏移（光标合成用）
    int MonitorY() const { return monitor_y_; }                        // 显示器屏幕 Y 偏移（光标合成用）

    // 静态方法：枚举所有显示器
    static std::vector<MonitorInfo> EnumerateMonitors();

private:
    DisplayCaptureMethod ChooseMethod(HMONITOR monitor);                // 自动选择采集方法
    void FreeCaptureData();                                             // 释放采集资源（保留设备）
    void UpdateMonitorHandle();                                         // 重新查找显示器句柄

    // ---- 配置 ----
    ID3D11Device* device_{nullptr};                                     // D3D11 设备（外部拥有）
    char monitor_id_[128]{0};                                           // 目标显示器设备 ID
    DisplayCaptureMethod method_{DisplayCaptureMethod::Auto};           // 采集方法
    bool capture_cursor_{false};                                        // 是否采集光标
    bool force_sdr_{false};                                             // 是否强制 SDR
    HMONITOR handle_{nullptr};                                          // 显示器句柄

    // ---- 采集器实例 ----
    DxgiDuplicator dxgi_;                                               // DXGI Desktop Duplication 采集器
    WgcCapture wgc_;                                                    // WGC 采集器
    DcCapture gdi_;                                                     // GDI BitBlt 采集器（回退）
    CursorCapture cursor_;                                              // 光标采集器（DXGI 路线用）

    // ---- 状态 ----
    bool use_gdi_{false};                                               // 是否使用 GDI 回退路线
    bool showing_{false};                                               // 源是否正在显示
    float reset_timeout_{0.0f};                                         // 重置超时计时器
    uint32_t width_{0};                                                 // 帧宽度
    uint32_t height_{0};                                                // 帧高度
    int rotation_{0};                                                   // 旋转角度
    int monitor_x_{0};                                                  // 显示器 X 偏移
    int monitor_y_{0};                                                  // 显示器 Y 偏移
    bool reset_wgc_{false};                                             // 是否需要重置 WGC
};

#endif // MONITORCAPTURE_H
