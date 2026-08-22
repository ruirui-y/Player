#include "CursorCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <QDebug>

// ==========
// 用 DrawIconEx 将光标绘制到 BGRA 位图
//
// 为什么是这个方案：
//   旧方案用 GetIconInfo + 手动解析 ICONINFO 的 hbmColor/hbmMask 位图，
//   需要处理彩色/单色/Alpha/无Alpha 四种情况，容易出错。
//   DrawIconEx 是 Windows 系统光标渲染函数，内部处理了所有格式，
//   直接输出到 32-bit BGRA 位图，比手动解析可靠得多。
// 底层是什么：
//   DrawIconEx → GDI32.dll → win32kfull.sys 内核光标渲染管线
//   内核知道当前光标的所有格式细节，不需要用户态手动解析
// ==========

bool CursorCapture::CaptureViaDrawIconEx(HICON icon, uint32_t& out_w, uint32_t& out_h,
                                         std::vector<uint8_t>& out_bgra)
{
    if (!icon)
        return false;

    // ---- 第一步：获取光标尺寸和热点 ----
    ICONINFO ii;
    if (!GetIconInfo(icon, &ii))
        return false;

    BITMAP bm;
    if (ii.hbmColor)
    {
        GetObject(ii.hbmColor, sizeof(bm), &bm);
    }
    else if (ii.hbmMask)
    {
        GetObject(ii.hbmMask, sizeof(bm), &bm);
        bm.bmHeight /= 2;                                            // 单色掩码高度是两倍
    }
    else
    {
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
        return false;
    }

    int w = bm.bmWidth;
    int h = bm.bmHeight;

    if (w <= 0 || h <= 0)
    {
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
        return false;
    }

    x_hotspot_ = ii.xHotspot;
    y_hotspot_ = ii.yHotspot;

    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);

    // ---- 第二步：创建 32-bit BGRA 内存位图和 DC ----
    HDC screen_dc = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen_dc);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;                                     // 负值 = 自上而下（BGRA 顺序）
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint8_t* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS,
                                   reinterpret_cast<void**>(&bits), nullptr, 0);
    if (!dib || !bits)
    {
        DeleteDC(mem_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, dib));

    // ---- 第三步：用 DrawIconEx 绘制光标到内存 DC ----
    // DI_NORMAL：绘制标准图标（无特殊效果）
    // DrawIconEx 内部处理 Alpha 混合、单色掩码、彩色位图等所有格式
    if (!DrawIconEx(mem_dc, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL))
    {
        SelectObject(mem_dc, old_bmp);
        DeleteObject(dib);
        DeleteDC(mem_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    // ---- 第四步：GDI 预乘 Alpha 反转（GDI 输出预乘 Alpha，需要转为直通 Alpha） ----
    // DrawIconEx 输出的 BGRA 是预乘 Alpha 格式（R' = R * A / 255）
    // 我们的合成代码需要直通 Alpha 格式
    // 参考：https://devblogs.microsoft.com/oldnewthing/20101008-00/?p=12593
    out_bgra.resize(static_cast<size_t>(w) * h * 4);
    memcpy(out_bgra.data(), bits, out_bgra.size());

    for (int i = 0; i < w * h; i++)
    {
        uint8_t* px = out_bgra.data() + i * 4;
        uint8_t alpha = px[3];
        if (alpha > 0 && alpha < 255)
        {
            // 反转预乘：R = R' * 255 / A
            px[0] = static_cast<uint8_t>((static_cast<int>(px[0]) * 255) / alpha);
            px[1] = static_cast<uint8_t>((static_cast<int>(px[1]) * 255) / alpha);
            px[2] = static_cast<uint8_t>((static_cast<int>(px[2]) * 255) / alpha);
        }
    }

    out_w = static_cast<uint32_t>(w);
    out_h = static_cast<uint32_t>(h);

    // ---- 第五步：清理 ----
    SelectObject(mem_dc, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);

    return true;
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
        qDebug() << "[CursorCapture] GetCursorInfo 失败, err =" << GetLastError();
        visible_ = false;
        return;
    }

    cursor_pos_ = ci.ptScreenPos;

    // ---- 每帧都更新可见性（CURSOR_SHOWING 标志位） ----
    bool is_showing = ((ci.flags & CURSOR_SHOWING) != 0);

    if (!is_showing)
    {
        // 光标被系统隐藏（如编辑文本时 Windows 隐藏光标）
        qDebug() << "[CursorCapture] CURSOR_SHOWING=0  pos=(" << cursor_pos_.x << "," << cursor_pos_.y
                 << ") hCursor=0x" << Qt::hex << (quintptr)ci.hCursor << "-> 不可见";
        visible_ = false;
        return;
    }

    // ---- 光标可见 → 尝试捕获位图 ----
    // 不缓存句柄：每次都用 DrawIconEx 重新绘制，避免句柄比较导致的卡死
    HICON icon = CopyIcon(ci.hCursor);
    if (icon)
    {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> bgra;
        if (CaptureViaDrawIconEx(icon, w, h, bgra))
        {
            bitmap_w_ = w;
            bitmap_h_ = h;
            bitmap_data_ = std::move(bgra);
            visible_ = true;
        }
        else
        {
            // DrawIconEx 失败 → 保留旧位图，但标记不可见
            // 下一帧会重试
            qDebug() << "[CursorCapture] DrawIconEx 失败 → 保留旧位图，标记不可见";
            visible_ = false;
        }
        DestroyIcon(icon);
    }
    else
    {
        qDebug() << "[CursorCapture] CopyIcon 失败 hCursor=0x" << Qt::hex << (quintptr)ci.hCursor
                 << "err =" << GetLastError();
        visible_ = false;
    }
}

// ========== 获取光标渲染信息 ==========

bool CursorCapture::GetInfo(CursorInfo& out_info) const
{
    if (!visible_ || hidden_ || bitmap_data_.empty())
    {
        // qDebug() << "[CursorCapture] 获取光标信息失败，光标不可见或被隐藏";
        return false;
    }

    out_info.bitmap = bitmap_data_.data();
    out_info.width = bitmap_w_;
    out_info.height = bitmap_h_;
    out_info.x_hotspot = x_hotspot_;
    out_info.y_hotspot = y_hotspot_;
    out_info.screen_x = cursor_pos_.x;
    out_info.screen_y = cursor_pos_.y;
    out_info.visible = true;
    out_info.monochrome = false;

    return true;
}