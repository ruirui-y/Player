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
    struct CachedCursor                                                 // 缓存的光标位图
    {
        uint32_t width;                                                 // 位图宽度
        uint32_t height;                                                // 位图高度
        std::vector<uint8_t> data;                                      // BGRA 位图数据
    };

    HCURSOR current_cursor_{nullptr};                                   // 当前光标句柄（用于检测变化）
    POINT cursor_pos_{0, 0};                                            // 光标屏幕坐标
    long x_hotspot_{0};                                                 // 热点 X
    long y_hotspot_{0};                                                 // 热点 Y
    bool visible_{false};                                               // 是否可见
    bool monochrome_{false};                                            // 是否单色
    bool hidden_{false};                                                // 是否被外部强制隐藏

    uint32_t last_cx_{0};                                               // 上次缓存的宽度
    uint32_t last_cy_{0};                                               // 上次缓存的高度
    std::vector<CachedCursor> cached_textures_;                         // 光标位图缓存池

    CachedCursor* GetCachedTexture(uint32_t cx, uint32_t cy);           // 按尺寸获取缓存位图
    bool CaptureIcon(HICON icon);                                       // 解析图标为 BGRA 位图并写入缓存
    static std::vector<uint8_t> CopyFromColor(ICONINFO& ii,             // 从彩色位图提取 BGRA
                                              uint32_t& width, uint32_t& height);
    static std::vector<uint8_t> CopyFromMask(ICONINFO& ii,              // 从单色掩码提取 BGRA
                                             uint32_t& width, uint32_t& height);
    static std::vector<uint8_t> GetBitmapData(HBITMAP hbmp, BITMAP& bmp);   // 读取 GDI 位图原始数据
    static void ApplyMask(uint8_t* color, const uint8_t* mask,          // 将 AND 掩码应用到 Alpha 通道
                          const BITMAP& bmp_mask);
};

#endif // CURSORCAPTURE_H
