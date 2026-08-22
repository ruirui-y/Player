#ifndef MONITORCAPTURE_H
#define MONITORCAPTURE_H

#include "CaptureBackend.h"
#include "CaptureCommon.h"
#include "CursorCapture.h"

#include <chrono>
#include <memory>
#include <vector>

class MonitorCapture
{
public:
    MonitorCapture();                                                                   // 构造
    ~MonitorCapture();                                                                  // 析构

    // 初始化，传入 D3D11 设备、显示器 ID、采集方法和选项
    // device 为 nullptr 时自动降级到 GDI 路线
    bool Init(ID3D11Device* device, const char* monitor_id,
              DisplayCaptureMethod method, bool capture_cursor, bool force_sdr);

    void Shutdown();                                                                    // 停止采集并释放资源

    // 采集一帧（内部自愈：后端失效时按 3 秒退避重建），成功则 out_frame 有效
    bool Capture(CaptureFrame& out_frame);

    // 获取光标信息（仅 DXGI 路线需要外部叠加光标，WGC 和 GDI 内部已处理）
    bool GetCursorInfo(CursorInfo& out_info);

    uint32_t Width() const;                                                             // 帧宽度（考虑旋转）
    uint32_t Height() const;                                                            // 帧高度（考虑旋转）
    DisplayCaptureMethod Method() const;                                                // 当前实际生效的采集方法
    bool IsActive() const;                                                              // 是否正在采集
    int MonitorX() const { return rect_.left; }                                         // 显示器屏幕 X 偏移（WGC/DXGI/GDI 通用）
    int MonitorY() const { return rect_.top; }                                          // 显示器屏幕 Y 偏移（WGC/DXGI/GDI 通用）

    // 静态方法：枚举所有显示器
    static std::vector<MonitorInfo> EnumerateMonitors();

private:
    DisplayCaptureMethod ChooseMethod(HMONITOR monitor);                                // 自动选择采集方法（纯函数）
    bool IsLaptopDualGpu() const;                                                       // 笔记本电池+双显卡判定
    void EnsureBackend();                                                               // 创建/切换后端 + 3 秒退避重建
    void FreeCaptureData();                                                             // 释放采集资源（保留设备）
    void UpdateMonitorHandle();                                                         // 重新查找显示器句柄

    // ---- 配置 ----
    ID3D11Device* device_{nullptr};                                                     // D3D11 设备（外部拥有）
    char monitor_id_[128]{0};                                                           // 目标显示器设备 ID
    DisplayCaptureMethod method_{DisplayCaptureMethod::Auto};                           // 用户请求的采集方法
    DisplayCaptureMethod active_method_{DisplayCaptureMethod::Auto};                    // 实际生效的采集方法（含 Gdi 回退）
    bool capture_cursor_{false};                                                        // 是否采集光标
    bool force_sdr_{false};                                                             // 是否强制 SDR
    HMONITOR handle_{nullptr};                                                          // 显示器句柄
    RECT rect_{0, 0, 0, 0};                                                             // 显示器矩形（GDI 后端用）

    // ---- 采集后端 ----
    BackendContext ctx_;                                                                // 后端初始化参数
    std::unique_ptr<CaptureBackend> backend_;                                           // 当前生效的采集后端
    CursorCapture cursor_;                                                              // 光标采集器（DXGI 路线用）

    // ---- 状态 ----
    bool showing_{false};                                                               // 源是否正在显示
    uint32_t width_{0};                                                                 // 帧宽度
    uint32_t height_{0};                                                                // 帧高度
    int rotation_{0};                                                                   // 旋转角度
    std::chrono::steady_clock::time_point next_retry_{};                                // 后端重建退避截止时间（{} = 可立即重试）
};

#endif // MONITORCAPTURE_H
