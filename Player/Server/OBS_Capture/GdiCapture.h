#ifndef GDICAPTURE_H
#define GDICAPTURE_H

#include "CaptureBackend.h"
#include "CursorCapture.h"

#include <cstdint>

// ========== GDI BitBlt 采集器（CPU 路线） ==========
// 直接实现 CaptureBackend 接口；Monitor 与 Window 两条路线共用。

class GdiCapture : public CaptureBackend
{
public:
    GdiCapture();                                                                           // 构造
    ~GdiCapture();                                                                          // 析构

    // ---- CaptureBackend 接口 ----
    bool Init(const BackendContext& ctx) override;                                          // 从 ctx.rect / ctx.capture_cursor / ctx.window 初始化
    bool AcquireFrame() override;                                                           // 执行 BitBlt 采集（桌面或窗口）
    bool GetFrame(CaptureFrame& out) override;                                              // 取出最新帧（CPU BGRA）
    bool IsActive() const override;                                                         // 是否初始化成功
    void Shutdown() override;                                                               // 释放所有 GDI 资源
    DisplayCaptureMethod Kind() const override { return DisplayCaptureMethod::Gdi; }
    uint32_t Width() const override;
    uint32_t Height() const override;
    void SetCursorHidden(bool hidden) override;                                             // 前台进程检测：隐藏/显示光标

private:
    void InitDibSection();                                                                  // 创建兼容 DC 和 DIB Section
    void DrawCursorOnHdc(HDC hdc);                                                          // 在 DC 上叠加光标（用 window_ 计算坐标）

    int x_{0};                                                                              // 采集起点 X（桌面坐标）
    int y_{0};                                                                              // 采集起点 Y（桌面坐标）
    uint32_t width_{0};                                                                     // 采集宽度
    uint32_t height_{0};                                                                    // 采集高度
    bool capture_cursor_{false};                                                            // 是否采集光标
    bool valid_{false};                                                                     // 是否有效
    bool texture_written_{false};                                                           // 是否已写入新帧

    HWND window_{nullptr};                                                                  // 采集目标窗口（nullptr=桌面）
    HDC hdc_{nullptr};                                                                      // 兼容 DC
    HBITMAP bmp_{nullptr};                                                                  // DIB Section 位图
    HBITMAP old_bmp_{nullptr};                                                              // 原始位图（用于恢复）
    uint8_t* bits_{nullptr};                                                                // DIB Section 数据指针

    CursorCapture cursor_;                                                                  // 光标采集器
    bool cursor_hidden_{false};                                                             // 光标是否被隐藏
};

#endif // GDICAPTURE_H