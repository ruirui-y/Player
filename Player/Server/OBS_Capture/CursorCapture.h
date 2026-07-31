#ifndef CURSORCAPTURE_H
#define CURSORCAPTURE_H

#include "CaptureCommon.h"

#include <cstdint>
#include <vector>

class CursorCapture
{
public:
    CursorCapture();                                                    // 构造
    ~CursorCapture();                                                   // 析构

    void Capture();                                                     // 采集当前光标状态，内部判断是否需要更新位图

    bool GetInfo(CursorInfo& out_info) const;                           // 获取光标渲染信息，返回 false 表示不可见

    void SetHidden(bool hidden);                                        // 设置光标是否被隐藏（窗口采集前台检测用）

private:
    // 用 DrawIconEx 将光标绘制到 BGRA 位图（比手动解析 ICONINFO 更可靠）
    // DrawIconEx 是 Windows 系统光标渲染函数，能正确处理彩色/单色/Alpha/动画光标
    bool CaptureViaDrawIconEx(HICON icon, uint32_t& out_w, uint32_t& out_h,
                              std::vector<uint8_t>& out_bgra);          // DrawIconEx 方式捕获

    POINT cursor_pos_{0, 0};                                            // 光标屏幕坐标
    long x_hotspot_{0};                                                 // 热点 X
    long y_hotspot_{0};                                                 // 热点 Y
    bool visible_{false};                                               // 是否可见
    bool hidden_{false};                                                // 是否被外部强制隐藏

    uint32_t bitmap_w_{0};                                              // 当前位图宽度
    uint32_t bitmap_h_{0};                                              // 当前位图高度
    std::vector<uint8_t> bitmap_data_;                                  // 当前 BGRA 位图数据
};

#endif // CURSORCAPTURE_H