#include "DcCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ========== 创建兼容 DC 和 DIB Section ==========

void DcCapture::InitDibSection()
{
    BITMAPINFO bi = {0};
    BITMAPINFOHEADER* bih = &bi.bmiHeader;
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biBitCount = 32;                                               // BGRA 32 位
    bih->biWidth = static_cast<LONG>(width_);
    bih->biHeight = static_cast<LONG>(height_);                        // 正数 → 自下而上（需要垂直翻转）
    bih->biPlanes = 1;

    hdc_ = CreateCompatibleDC(nullptr);
    if (!hdc_)
    {
        return;
    }

    bmp_ = CreateDIBSection(hdc_, &bi, DIB_RGB_COLORS,
                            reinterpret_cast<void**>(&bits_), nullptr, 0);
    if (!bmp_)
    {
        DeleteDC(hdc_);
        hdc_ = nullptr;
        return;
    }

    old_bmp_ = static_cast<HBITMAP>(SelectObject(hdc_, bmp_));
    valid_ = true;
}

// ========== 构造 / 析构 ==========

DcCapture::DcCapture()
{
}

DcCapture::~DcCapture()
{
    Free();
}

// ========== 初始化采集区域 ==========

void DcCapture::Init(int x, int y, uint32_t width, uint32_t height, bool capture_cursor)
{
    Free();

    x_ = x;
    y_ = y;
    width_ = width;
    height_ = height;
    capture_cursor_ = capture_cursor;

    InitDibSection();
}

// ========== 释放所有 GDI 资源 ==========

void DcCapture::Free()
{
    if (hdc_)
    {
        SelectObject(hdc_, old_bmp_);
        DeleteDC(hdc_);
        hdc_ = nullptr;
    }

    if (bmp_)
    {
        DeleteObject(bmp_);
        bmp_ = nullptr;
    }

    old_bmp_ = nullptr;
    bits_ = nullptr;
    valid_ = false;
    texture_written_ = false;
    width_ = 0;
    height_ = 0;
}

// ========== 在 DC 上叠加光标 ==========

void DcCapture::DrawCursorOnHdc(HDC hdc, HWND window)
{
    if (cursor_hidden_)
    {
        return;
    }

    CURSORINFO ci = {0};
    ci.cbSize = sizeof(CURSORINFO);

    if (!GetCursorInfo(&ci))
    {
        return;
    }

    if ((ci.flags & CURSOR_SHOWING) == 0)
    {
        return;
    }

    HICON icon = CopyIcon(ci.hCursor);
    if (!icon)
    {
        return;
    }

    ICONINFO ii;
    if (GetIconInfo(icon, &ii))
    {
        POINT win_pos = {x_, y_};

        // ---- 窗口采集需要将客户区坐标转为屏幕坐标 ----
        if (window)
        {
            ClientToScreen(window, &win_pos);
        }

        POINT pos;
        pos.x = ci.ptScreenPos.x - static_cast<int>(ii.xHotspot) - win_pos.x;
        pos.y = ci.ptScreenPos.y - static_cast<int>(ii.yHotspot) - win_pos.y;

        DrawIconEx(hdc, pos.x, pos.y, icon, 0, 0, 0, nullptr, DI_NORMAL);

        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
    }

    DestroyIcon(icon);
}

// ========== 执行 BitBlt 采集 ==========

void DcCapture::Capture(HWND window)
{
    if (!valid_ || !hdc_)
    {
        return;
    }

    // ---- 获取目标 DC（nullptr 表示整个桌面） ----
    HDC hdc_target = GetDC(window);
    if (!hdc_target)
    {
        return;
    }

    BitBlt(hdc_, 0, 0, static_cast<int>(width_), static_cast<int>(height_),
           hdc_target, x_, y_, SRCCOPY);

    ReleaseDC(window, hdc_target);

    // ---- 采集光标并直接叠加到 DC 上 ----
    if (capture_cursor_)
    {
        DrawCursorOnHdc(hdc_, window);
    }

    texture_written_ = true;
}

// ========== 查询方法 ==========

bool DcCapture::IsValid() const
{
    return valid_;
}

bool DcCapture::HasNewFrame() const
{
    return texture_written_;
}

uint32_t DcCapture::Width() const
{
    return width_;
}

uint32_t DcCapture::Height() const
{
    return height_;
}

// ========== 获取帧数据 ==========

bool DcCapture::GetFrame(const uint8_t*& out_data, uint32_t& out_stride)
{
    if (!valid_ || !texture_written_ || !bits_)
    {
        return false;
    }

    out_data = bits_;
    out_stride = width_ * 4;                                            // BGRA 每像素 4 字节

    // ---- 读取后标记为已消费，下一帧需要重新 Capture ----
    texture_written_ = false;

    return true;
}

// ========== 设置光标是否隐藏 ==========

void DcCapture::SetCursorHidden(bool hidden)
{
    cursor_hidden_ = hidden;
}
