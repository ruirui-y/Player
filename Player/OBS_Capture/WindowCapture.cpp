#include "WindowCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <cctype>

// ========== WGC 白名单：部分匹配（不区分大小写的子串匹配） ==========

static const char* WGC_PARTIAL_MATCH_CLASSES[] =
{
    "Chrome",
    "Mozilla",
    nullptr,
};

// ========== WGC 白名单：完全匹配（不区分大小写） ==========

static const char* WGC_WHOLE_MATCH_CLASSES[] =
{
    "ApplicationFrameWindow",                                           // UWP 窗口框架
    "Windows.UI.Core.CoreWindow",                                       // UWP 核心窗口
    "WinUIDesktopWin32WindowClass",                                     // WinUI 桌面窗口
    "GAMINGSERVICESUI_HOSTING_WINDOW_CLASS",                            // Xbox Game Services
    "XLMAIN",                                                           // Microsoft Excel
    "PPTFrameClass",                                                    // Microsoft PowerPoint
    "screenClass",                                                      // PowerPoint 幻灯片放映
    "PodiumParent",                                                     // PowerPoint 演讲者视图
    "OpusApp",                                                          // Microsoft Word
    "OMain",                                                            // Microsoft Access
    "Framework::CFrame",                                                // Microsoft OneNote
    "rctrl_renwnd32",                                                   // Microsoft Outlook
    "MSWinPub",                                                         // Microsoft Publisher
    "OfficeApp-Frame",                                                  // Microsoft 365
    "SDL_app",                                                          // SDL 应用
    nullptr,
};

// ========== 辅助：不区分大小写的字符串比较 ==========

static int StrCmpINoCase(const char* a, const char* b)
{
    while (*a && *b)
    {
        int ca = std::tolower(static_cast<unsigned char>(*a));
        int cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb)
        {
            return ca - cb;
        }
        ++a;
        ++b;
    }
    return *a - *b;
}

// ========== 辅助：不区分大小写的子串查找 ==========

static const char* StrStrINoCase(const char* haystack, const char* needle)
{
    if (!*needle)
    {
        return haystack;
    }

    size_t needle_len = strlen(needle);
    for (const char* p = haystack; *p; ++p)
    {
        if (StrCmpINoCase(p, needle) == 0 ||
            _strnicmp(p, needle, needle_len) == 0)
        {
            // ---- 检查 p 是否以 needle 开头 ----
            bool match = true;
            for (size_t i = 0; i < needle_len; ++i)
            {
                if (std::tolower(static_cast<unsigned char>(p[i])) !=
                    std::tolower(static_cast<unsigned char>(needle[i])))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return p;
            }
        }
    }
    return nullptr;
}

// ========== 静态方法：判断是否应使用 WGC ==========

bool WindowCapture::ShouldUseWgc(const char* class_name)
{
    if (!class_name)
    {
        return false;
    }

    // ---- 部分匹配（如 Chrome、Mozilla） ----
    for (int i = 0; WGC_PARTIAL_MATCH_CLASSES[i]; ++i)
    {
        if (StrStrINoCase(class_name, WGC_PARTIAL_MATCH_CLASSES[i]))
        {
            return true;
        }
    }

    // ---- 完全匹配（如 XLMAIN、OpusApp） ----
    for (int i = 0; WGC_WHOLE_MATCH_CLASSES[i]; ++i)
    {
        if (StrCmpINoCase(class_name, WGC_WHOLE_MATCH_CLASSES[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

// ========== 静态方法：获取窗口类名 ==========

bool WindowCapture::GetWindowClass(HWND window, char* class_name, size_t count)
{
    if (!window || !class_name || count == 0)
    {
        return false;
    }

    // ---- GetClassNameA 返回类名（不含 null 终止符的长度） ----
    int len = GetClassNameA(window, class_name, static_cast<int>(count));
    return len > 0;
}

// ========== 加载 DPI 感知函数指针 ==========

void WindowCapture::InitDpiFunctions()
{
    HMODULE user32 = GetModuleHandleW(L"User32.dll");
    if (!user32)
    {
        return;
    }

    set_thread_dpi_awareness_context_ =
        reinterpret_cast<DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
    get_thread_dpi_awareness_context_ =
        reinterpret_cast<DPI_AWARENESS_CONTEXT(WINAPI*)()>(
            GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
    get_window_dpi_awareness_context_ =
        reinterpret_cast<DPI_AWARENESS_CONTEXT(WINAPI*)(HWND)>(
            GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
}

// ========== 自动选择采集方法 ==========

WindowCaptureMethod WindowCapture::ChooseMethod(const char* class_name)
{
    // ---- 没有 D3D11 设备或 WGC 不支持 → BitBlt ----
    if (!device_ || !WgcCapture::IsSupported())
    {
        return WindowCaptureMethod::BitBlt;
    }

    // ---- 用户手动指定时直接使用 ----
    if (method_ != WindowCaptureMethod::Auto)
    {
        return method_;
    }

    // ---- AUTO 模式：按白名单匹配决定 WGC 或 BitBlt ----
    if (ShouldUseWgc(class_name))
    {
        return WindowCaptureMethod::Wgc;
    }

    return WindowCaptureMethod::BitBlt;
}

// ========== 重置采集状态 ==========

void WindowCapture::ForceReset()
{
    window_ = nullptr;
    resize_timer_ = RESIZE_CHECK_TIME;
    check_window_timer_ = WC_CHECK_TIMER;
    cursor_check_time_ = CURSOR_CHECK_TIME;
    previously_failed_ = false;
}

// ========== 窗口是否正常（存在且非最小化） ==========

bool WindowCapture::WindowNormal() const
{
    return window_ && IsWindow(window_) && !IsIconic(window_);
}

// ========== 构造 / 析构 ==========

WindowCapture::WindowCapture()
{
}

WindowCapture::~WindowCapture()
{
    Shutdown();
}

// ========== 初始化 ==========

bool WindowCapture::Init(ID3D11Device* device, HWND window, WindowCaptureMethod method,
                         bool cursor, bool compatibility, bool client_area, bool force_sdr)
{
    Shutdown();

    device_ = device;
    window_ = window;
    method_ = method;
    cursor_ = cursor;
    compatibility_ = compatibility;
    client_area_ = client_area;
    force_sdr_ = force_sdr;

    InitDpiFunctions();
    ForceReset();

    return true;
}

// ========== 停止采集并释放资源 ==========

void WindowCapture::Shutdown()
{
    wgc_capture_.Shutdown();
    dc_capture_.Free();

    device_ = nullptr;
    window_ = nullptr;
    hooked_ = false;
    previously_failed_ = false;

    set_thread_dpi_awareness_context_ = nullptr;
    get_thread_dpi_awareness_context_ = nullptr;
    get_window_dpi_awareness_context_ = nullptr;
}

// ========== 每帧调用 ==========

void WindowCapture::Tick(float delta_seconds)
{
    RECT rect;
    bool reset_capture = false;

    // ---- 窗口不存在时等待重试 ----
    if (!window_ || !IsWindow(window_))
    {
        if (hooked_)
        {
            hooked_ = false;
        }

        // ---- 释放采集资源 ----
        if (dc_capture_.IsValid())
        {
            dc_capture_.Free();
        }

        // ---- 每秒检查一次窗口是否存在 ----
        check_window_timer_ += delta_seconds;
        if (check_window_timer_ < WC_CHECK_TIMER)
        {
            return;
        }

        // ---- 释放 WGC 实例 ----
        if (wgc_capture_.IsActive())
        {
            wgc_capture_.Shutdown();
        }

        check_window_timer_ = 0.0f;

        // ---- 注意：此实现中 window_ 由外部传入，不自动查找 ----
        // 如果需要自动查找窗口，调用方应在窗口重新出现后调用 Init()
        return;
    }

    // ---- 窗口最小化或不可见时跳过（WGC 无法初始化） ----
    if (IsIconic(window_) || !IsWindowVisible(window_))
    {
        return;
    }

    // ---- 光标显隐检测（每 0.2 秒） ----
    cursor_check_time_ += delta_seconds;
    if (cursor_check_time_ >= CURSOR_CHECK_TIME)
    {
        DWORD foreground_pid = 0;
        DWORD target_pid = 0;

        // ---- 比较前台窗口和目标窗口的进程 ID ----
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        GetWindowThreadProcessId(window_, &target_pid);

        // ---- 前台进程 != 目标进程 → 隐藏光标 ----
        bool cursor_hidden = foreground_pid && target_pid && foreground_pid != target_pid;
        dc_capture_.SetCursorHidden(cursor_hidden);

        // ---- WGC 路线动态切换光标 ----
        if (wgc_capture_.IsActive())
        {
            if (!wgc_capture_.ShowCursor(!cursor_hidden))
            {
                // ---- ShowCursor 失败 → 需要重置 ----
                ForceReset();
                hooked_ = false;
                return;
            }
        }

        cursor_check_time_ = 0.0f;
    }

    // ---- 获取窗口类名用于路线选择 ----
    char class_name[256] = {0};
    GetWindowClass(window_, class_name, _countof(class_name));

    WindowCaptureMethod chosen = ChooseMethod(class_name);

    // ========== BitBlt 路线 ==========
    if (chosen == WindowCaptureMethod::BitBlt)
    {
        // ---- 释放可能存在的 WGC 实例 ----
        if (wgc_capture_.IsActive())
        {
            wgc_capture_.Shutdown();
        }

        // ---- 设置 DPI 感知上下文，确保 GetClientRect 返回正确尺寸 ----
        DPI_AWARENESS_CONTEXT previous = nullptr;
        if (get_window_dpi_awareness_context_)
        {
            DPI_AWARENESS_CONTEXT ctx = get_window_dpi_awareness_context_(window_);
            previous = set_thread_dpi_awareness_context_(ctx);
        }

        GetClientRect(window_, &rect);

        // ---- 检测窗口尺寸是否变化（每 0.2 秒检测一次） ----
        if (!reset_capture)
        {
            resize_timer_ += delta_seconds;

            if (resize_timer_ >= RESIZE_CHECK_TIME)
            {
                if ((rect.bottom - rect.top) != (last_rect_.bottom - last_rect_.top) ||
                    (rect.right - rect.left) != (last_rect_.right - last_rect_.left))
                {
                    reset_capture = true;
                }

                resize_timer_ = 0.0f;
            }
        }

        // ---- 尺寸变化或首次 → 重新初始化 DcCapture ----
        if (reset_capture)
        {
            resize_timer_ = 0.0f;
            last_rect_ = rect;

            uint32_t w = rect.right - rect.left;
            uint32_t h = rect.bottom - rect.top;

            dc_capture_.Free();
            dc_capture_.Init(0, 0, w, h, cursor_);

            if (!hooked_ && dc_capture_.IsValid())
            {
                hooked_ = true;
            }
        }

        // ---- 执行 BitBlt 采集 ----
        dc_capture_.Capture(window_);

        // ---- 恢复 DPI 感知上下文 ----
        if (previous && set_thread_dpi_awareness_context_)
        {
            set_thread_dpi_awareness_context_(previous);
        }
    }
    // ========== WGC 路线 ==========
    else if (chosen == WindowCaptureMethod::Wgc)
    {
        // ---- 释放可能存在的 BitBlt 资源 ----
        if (dc_capture_.IsValid())
        {
            dc_capture_.Free();
        }

        // ---- WGC 实例不存在且未失败过 → 尝试初始化 ----
        if (window_ && !wgc_capture_.IsActive() && !previously_failed_)
        {
            if (!wgc_capture_.InitWindow(device_, window_, client_area_,
                                          cursor_, force_sdr_))
            {
                // ---- 初始化失败 → 标记不再重试 ----
                previously_failed_ = true;
            }
            else
            {
                hooked_ = true;
            }
        }

        // ---- 有活跃实例时检查帧 ----
        if (wgc_capture_.IsActive())
        {
            wgc_capture_.Tick();
        }
    }
}

// ========== 获取当前帧 ==========

bool WindowCapture::GetFrame(CaptureFrame& out_frame)
{
    if (!WindowNormal())
    {
        return false;
    }

    // ---- WGC 路线：返回 GPU 纹理 ----
    if (wgc_capture_.IsActive())
    {
        ID3D11Texture2D* tex = wgc_capture_.GetTexture();
        if (tex)
        {
            out_frame.gpu_texture = tex;
            out_frame.cpu_data = nullptr;
            out_frame.width = wgc_capture_.Width();
            out_frame.height = wgc_capture_.Height();
            out_frame.rotation = 0;
            return true;
        }
        return false;
    }

    // ---- BitBlt 路线：返回 CPU 内存数据 ----
    const uint8_t* data = nullptr;
    uint32_t stride = 0;
    if (dc_capture_.GetFrame(data, stride))
    {
        out_frame.gpu_texture = nullptr;
        out_frame.cpu_data = data;
        out_frame.cpu_stride = stride;
        out_frame.width = dc_capture_.Width();
        out_frame.height = dc_capture_.Height();
        out_frame.rotation = 0;
        return true;
    }

    return false;
}

// ========== 查询方法 ==========

uint32_t WindowCapture::Width() const
{
    if (!WindowNormal())
    {
        return 0;
    }

    if (wgc_capture_.IsActive())
    {
        return wgc_capture_.Width();
    }

    return dc_capture_.Width();
}

uint32_t WindowCapture::Height() const
{
    if (!WindowNormal())
    {
        return 0;
    }

    if (wgc_capture_.IsActive())
    {
        return wgc_capture_.Height();
    }

    return dc_capture_.Height();
}

WindowCaptureMethod WindowCapture::Method() const
{
    return method_;
}

bool WindowCapture::IsHooked() const
{
    return hooked_;
}
