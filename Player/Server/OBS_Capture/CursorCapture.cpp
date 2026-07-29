#include "CursorCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ========== 辅助函数：读取 GDI 位图原始数据 ==========

std::vector<uint8_t> CursorCapture::GetBitmapData(HBITMAP hbmp, BITMAP& bmp)
{
    if (GetObject(hbmp, sizeof(bmp), &bmp) == 0)
    {
        return {};
    }

    unsigned int size = bmp.bmHeight * bmp.bmWidthBytes;
    if (size == 0)
    {
        return {};
    }

    std::vector<uint8_t> output(size);
    if (GetBitmapBits(hbmp, size, output.data()) == 0)
    {
        return {};
    }

    return output;
}

// ========== 辅助函数：位掩码转 Alpha ==========

static inline uint8_t BitToAlpha(const uint8_t* data, long pixel, bool invert)
{
    uint8_t pix_byte = data[pixel / 8];
    bool alpha = (pix_byte >> (7 - pixel % 8) & 1) != 0;

    if (invert)
    {
        return alpha ? 0xFF : 0;
    }
    else
    {
        return alpha ? 0 : 0xFF;
    }
}

// ========== 辅助函数：检测彩色位图是否有 Alpha 通道 ==========

static inline bool BitmapHasAlpha(const uint8_t* data, long num_pixels)
{
    for (long i = 0; i < num_pixels; i++)
    {
        if (data[i * 4 + 3] != 0)
        {
            return true;
        }
    }

    return false;
}

// ========== 辅助函数：将 AND 掩码应用到 Alpha 通道 ==========

void CursorCapture::ApplyMask(uint8_t* color, const uint8_t* mask, const BITMAP& bmp_mask)
{
    for (long y = 0; y < bmp_mask.bmHeight; y++)
    {
        for (long x = 0; x < bmp_mask.bmWidth; x++)
        {
            long mask_pix_offs = y * (bmp_mask.bmWidthBytes * 8) + x;
            color[(y * bmp_mask.bmWidth + x) * 4 + 3] = BitToAlpha(mask, mask_pix_offs, false);
        }
    }
}

// ========== 从彩色位图提取 BGRA ==========

std::vector<uint8_t> CursorCapture::CopyFromColor(ICONINFO& ii, uint32_t& width, uint32_t& height)
{
    BITMAP bmp_color;
    BITMAP bmp_mask;

    std::vector<uint8_t> color = GetBitmapData(ii.hbmColor, bmp_color);
    if (color.empty())
    {
        return {};
    }

    // 低于 32 位 → 无法获取 Alpha，放弃
    if (bmp_color.bmBitsPixel < 32)
    {
        return {};
    }

    // 尝试读取掩码，若彩色位图没有 Alpha 则用掩码补全
    std::vector<uint8_t> mask = GetBitmapData(ii.hbmMask, bmp_mask);
    if (!mask.empty())
    {
        long pixels = bmp_color.bmHeight * bmp_color.bmWidth;

        if (!BitmapHasAlpha(color.data(), pixels))
        {
            ApplyMask(color.data(), mask.data(), bmp_mask);
        }
    }

    width = bmp_color.bmWidth;
    height = bmp_color.bmHeight;
    return color;
}

// ========== 从单色掩码提取 BGRA ==========

std::vector<uint8_t> CursorCapture::CopyFromMask(ICONINFO& ii, uint32_t& width, uint32_t& height)
{
    BITMAP bmp;

    std::vector<uint8_t> mask = GetBitmapData(ii.hbmMask, bmp);
    if (mask.empty())
    {
        return {};
    }

    // 单色光标的掩码高度是两倍：上半 AND 掩码，下半 XOR 掩码
    bmp.bmHeight /= 2;

    long pixels = bmp.bmHeight * bmp.bmWidth;
    if (pixels == 0)
    {
        return {};
    }

    std::vector<uint8_t> output(pixels * 4);

    long bottom = bmp.bmWidthBytes * bmp.bmHeight;

    // ---- 逐像素：AND 掩码决定透明度，XOR 掩码决定颜色 ----
    for (long i = 0; i < pixels; i++)
    {
        uint8_t and_mask = BitToAlpha(mask.data(), i, true);
        uint8_t xor_mask = BitToAlpha(mask.data() + bottom, i, true);

        if (!and_mask)
        {
            // AND 掩码为黑色 → 不透明
            *reinterpret_cast<uint32_t*>(&output[i * 4]) = xor_mask ? 0x00FFFFFF : 0xFF000000;
        }
        else
        {
            // AND 掩码为白色 → 透明或反色
            *reinterpret_cast<uint32_t*>(&output[i * 4]) = xor_mask ? 0xFFFFFFFF : 0;
        }
    }

    width = bmp.bmWidth;
    height = bmp.bmHeight;
    return output;
}

// ========== 按尺寸获取缓存位图 ==========

CursorCapture::CachedCursor* CursorCapture::GetCachedTexture(uint32_t cx, uint32_t cy)
{
    // ---- 先查找已缓存的同尺寸位图 ----
    for (auto& cc : cached_textures_)
    {
        if (cc.width == cx && cc.height == cy)
        {
            return &cc;
        }
    }

    // ---- 没找到 → 新建一个缓存项 ----
    CachedCursor cc;
    cc.width = cx;
    cc.height = cy;
    cc.data.resize(cx * cy * 4);
    cached_textures_.push_back(std::move(cc));

    return &cached_textures_.back();
}

// ========== 解析图标为 BGRA 位图 ==========

bool CursorCapture::CaptureIcon(HICON icon)
{
    if (!icon)
    {
        return false;
    }

    ICONINFO ii;
    if (!GetIconInfo(icon, &ii))
    {
        return false;
    }

    // ---- 先尝试彩色位图，失败再尝试单色掩码 ----
    uint32_t width = 0;
    uint32_t height = 0;
    monochrome_ = false;

    std::vector<uint8_t> bitmap = CopyFromColor(ii, width, height);
    if (bitmap.empty())
    {
        monochrome_ = true;
        bitmap = CopyFromMask(ii, width, height);
    }

    if (!bitmap.empty())
    {
        // ---- 尺寸变化时切换缓存槽位 ----
        if (last_cx_ != width || last_cy_ != height)
        {
            // GetCachedTexture 的调用在下面
            last_cx_ = width;
            last_cy_ = height;
        }

        CachedCursor* cc = GetCachedTexture(width, height);
        if (cc)
        {
            memcpy(cc->data.data(), bitmap.data(), bitmap.size());
        }

        x_hotspot_ = ii.xHotspot;
        y_hotspot_ = ii.yHotspot;
    }

    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);

    return !bitmap.empty();
}

// ========== 构造 / 析构 ==========

CursorCapture::CursorCapture()
{
}

CursorCapture::~CursorCapture()
{
}

// ========== 设置光标是否被外部隐藏 ==========

void CursorCapture::SetHidden(bool hidden)
{
    hidden_ = hidden;
}

// ========== 采集当前光标状态 ==========

void CursorCapture::Capture()
{
    CURSORINFO ci = {0};
    ci.cbSize = sizeof(ci);

    if (!GetCursorInfo(&ci))
    {
        visible_ = false;
        return;
    }

    cursor_pos_ = ci.ptScreenPos;

    // ---- 光标句柄没变 → 不需要重新解析位图 ----
    if (current_cursor_ == ci.hCursor)
    {
        return;
    }

    HICON icon = CopyIcon(ci.hCursor);
    bool has_bitmap = CaptureIcon(icon);
    current_cursor_ = ci.hCursor;

    // ---- CURSOR_SHOWING 标志位判断光标是否可见 ----
    visible_ = has_bitmap && ((ci.flags & CURSOR_SHOWING) != 0);

    DestroyIcon(icon);
}

// ========== 获取光标渲染信息 ==========

bool CursorCapture::GetInfo(CursorInfo& out_info) const
{
    if (!visible_ || hidden_)
    {
        return false;
    }

    // ---- 找到当前尺寸对应的缓存位图 ----
    for (const auto& cc : cached_textures_)
    {
        if (cc.width == last_cx_ && cc.height == last_cy_)
        {
            out_info.bitmap = cc.data.data();
            out_info.width = cc.width;
            out_info.height = cc.height;
            out_info.x_hotspot = x_hotspot_;
            out_info.y_hotspot = y_hotspot_;
            out_info.screen_x = cursor_pos_.x;
            out_info.screen_y = cursor_pos_.y;
            out_info.visible = true;
            out_info.monochrome = monochrome_;
            return true;
        }
    }

    return false;
}
