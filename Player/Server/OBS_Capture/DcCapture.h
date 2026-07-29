#ifndef DCCAPTURE_H
#define DCCAPTURE_H

#include "CaptureCommon.h"
#include "CursorCapture.h"

#include <cstdint>

class DcCapture
{
public:
    DcCapture();                                                        // 构造
    ~DcCapture();                                                       // 析构

    // 初始化采集区域，传入坐标和尺寸（显示器采集用绝对坐标，窗口采集传 0,0）
    void Init(int x, int y, uint32_t width, uint32_t height, bool capture_cursor);

    void Free();                                                        // 释放所有 GDI 资源

    // 执行 BitBlt 采集，window 传 nullptr 表示采集整个桌面
    void Capture(HWND window);

    bool IsValid() const;                                               // 是否初始化成功
    bool HasNewFrame() const;                                           // 是否有新帧未读取
    uint32_t Width() const;                                             // 采集宽度
    uint32_t Height() const;                                            // 采集高度

    // 获取帧数据，返回 CPU 内存 BGRA 指针和步幅
    bool GetFrame(const uint8_t*& out_data, uint32_t& out_stride);

    void SetCursorHidden(bool hidden);                                  // 设置光标是否隐藏（前台进程检测用）

private:
    void InitDibSection();                                              // 创建兼容 DC 和 DIB Section
    void DrawCursorOnHdc(HDC hdc, HWND window);                         // 在 DC 上叠加光标

    int x_{0};                                                          // 采集起点 X（桌面坐标）
    int y_{0};                                                          // 采集起点 Y（桌面坐标）
    uint32_t width_{0};                                                 // 采集宽度
    uint32_t height_{0};                                                // 采集高度
    bool capture_cursor_{false};                                        // 是否采集光标
    bool valid_{false};                                                 // 是否有效
    bool texture_written_{false};                                       // 是否已写入新帧

    HDC hdc_{nullptr};                                                  // 兼容 DC
    HBITMAP bmp_{nullptr};                                              // DIB Section 位图
    HBITMAP old_bmp_{nullptr};                                          // 原始位图（用于恢复）
    uint8_t* bits_{nullptr};                                            // DIB Section 数据指针

    CursorCapture cursor_;                                              // 光标采集器
    bool cursor_hidden_{false};                                         // 光标是否被隐藏
};

#endif // DCCAPTURE_H
