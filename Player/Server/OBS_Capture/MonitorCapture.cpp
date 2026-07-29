#include "MonitorCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <QDebug>

// ========== 辅助：显示器枚举回调数据 ==========

struct EnumMonitorContext
{
    std::vector<MonitorInfo>* monitors;                                // 输出列表
};

// ========== 辅助：通过 DISPLAYCONFIG 获取显示器友好名称 ==========

static bool GetMonitorTarget(LPCWSTR device, DISPLAYCONFIG_TARGET_DEVICE_NAME* target)
{
    bool found = false;

    UINT32 num_path, num_mode;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_path, &num_mode) != ERROR_SUCCESS)
    {
        return false;
    }

    if (num_path == 0 || num_mode == 0)
    {
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(num_path);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(num_mode);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_path, paths.data(),
                           &num_mode, modes.data(), nullptr) != ERROR_SUCCESS)
    {
        return false;
    }

    // ---- 遍历路径，匹配 GDI 设备名 ----
    for (size_t i = 0; i < num_path; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source;
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
            wcscmp(device, source.viewGdiDeviceName) == 0)
        {
            target->header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target->header.size = sizeof(*target);
            target->header.adapterId = paths[i].sourceInfo.adapterId;
            target->header.id = paths[i].targetInfo.id;
            found = DisplayConfigGetDeviceInfo(&target->header) == ERROR_SUCCESS;
            break;
        }
    }

    return found;
}

// ========== 实现 CaptureCommon.h 中声明的函数 ==========

bool GetMonitorName(HMONITOR handle, char* name, size_t count)
{
    MONITORINFOEXW mi;
    DISPLAYCONFIG_TARGET_DEVICE_NAME target;

    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(handle, reinterpret_cast<LPMONITORINFO>(&mi)) &&
        GetMonitorTarget(mi.szDevice, &target))
    {
        // ---- 将宽字符友好名称转为 UTF-8 ----
        WideCharToMultiByte(CP_UTF8, 0, target.monitorFriendlyDeviceName, -1,
                           name, static_cast<int>(count), nullptr, nullptr);
        return true;
    }

    strcpy_s(name, count, "[Unknown]");
    return false;
}

// ========== 显示器枚举回调 ==========

static BOOL CALLBACK EnumMonitorProc(HMONITOR handle, HDC /*hdc*/, LPRECT rect, LPARAM param)
{
    auto ctx = reinterpret_cast<EnumMonitorContext*>(param);

    MonitorInfo info;
    info.handle = handle;
    info.rect = *rect;

    MONITORINFOEXA mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(handle, reinterpret_cast<LPMONITORINFO>(&mi)))
    {
        DISPLAY_DEVICEA device;
        device.cb = sizeof(device);
        if (EnumDisplayDevicesA(mi.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME))
        {
            strcpy_s(info.device_id, _countof(info.device_id), device.DeviceID);
        }
        strcpy_s(info.alt_id, _countof(info.alt_id), mi.szDevice);
    }

    GetMonitorName(handle, info.name, _countof(info.name));

    ctx->monitors->push_back(info);
    return TRUE;
}

// ========== 静态方法：枚举所有显示器 ==========

std::vector<MonitorInfo> MonitorCapture::EnumerateMonitors()
{
    std::vector<MonitorInfo> monitors;
    EnumMonitorContext ctx;
    ctx.monitors = &monitors;

    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, reinterpret_cast<LPARAM>(&ctx));

    return monitors;
}

// ========== 实现 FindMonitorById ==========

static BOOL CALLBACK FindMonitorByIdProc(HMONITOR handle, HDC /*hdc*/, LPRECT rect, LPARAM param)
{
    auto info = reinterpret_cast<MonitorInfo*>(param);

    MONITORINFOEXA mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(handle, reinterpret_cast<LPMONITORINFO>(&mi)))
    {
        DISPLAY_DEVICEA device;
        device.cb = sizeof(device);
        if (EnumDisplayDevicesA(mi.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME))
        {
            if (strcmp(info->device_id, device.DeviceID) == 0)
            {
                strcpy_s(info->alt_id, _countof(info->alt_id), mi.szDevice);
                info->rect = *rect;
                info->handle = handle;
                GetMonitorName(handle, info->name, _countof(info->name));
                return FALSE;                                          // 找到了，停止枚举
            }
        }
    }

    return TRUE;                                                       // 继续枚举
}

bool FindMonitorById(const char* monitor_id, MonitorInfo& out_info)
{
    memset(&out_info, 0, sizeof(out_info));
    strcpy_s(out_info.device_id, _countof(out_info.device_id), monitor_id);

    EnumDisplayMonitors(nullptr, nullptr, FindMonitorByIdProc,
                        reinterpret_cast<LPARAM>(&out_info));

    // ---- 如果按设备 ID 没找到，尝试按 alt_id（szDevice）匹配 ----
    if (!out_info.handle)
    {
        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR handle, HDC, LPRECT rect, LPARAM param) -> BOOL
            {
                auto info = reinterpret_cast<MonitorInfo*>(param);
                MONITORINFOEXA mi;
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoA(handle, reinterpret_cast<LPMONITORINFO>(&mi)))
                {
                    if (strcmp(info->device_id, mi.szDevice) == 0)
                    {
                        strcpy_s(info->alt_id, _countof(info->alt_id), mi.szDevice);
                        info->rect = *rect;
                        info->handle = handle;
                        GetMonitorName(handle, info->name, _countof(info->name));
                        return FALSE;
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&out_info));
    }

    return out_info.handle != nullptr;
}

// ========== 构造 / 析构 ==========

MonitorCapture::MonitorCapture()
{
}

MonitorCapture::~MonitorCapture()
{
    Shutdown();
}

// ========== 自动选择采集方法 ==========

DisplayCaptureMethod MonitorCapture::ChooseMethod(HMONITOR monitor)
{
    // ---- 没有 D3D11 设备 → GDI 回退 ----
    if (!device_)
    {
        use_gdi_ = true;
        return DisplayCaptureMethod::Dxgi;                             // 返回值无意义，实际走 GDI
    }

    // ---- WGC 不支持 → 强制 DXGI ----
    if (!WgcCapture::IsSupported())
    {
        return DisplayCaptureMethod::Dxgi;
    }

    if (method_ == DisplayCaptureMethod::Auto)
    {
        // ---- AUTO 模式：默认 DXGI ----
        DisplayCaptureMethod result = DisplayCaptureMethod::Dxgi;

        // ---- DXGI 无法获取该显示器索引 → 切换 WGC ----
        int dxgi_index = DxgiDuplicator::GetMonitorIndex(monitor);
        if (dxgi_index == -1)
        {
            result = DisplayCaptureMethod::Wgc;
        }
        else
        {
            // ---- 笔记本电池 + 双显卡 → 切换 WGC（省电/兼容性） ----
            SYSTEM_POWER_STATUS status;
            if (GetSystemPowerStatus(&status) && status.BatteryFlag < 128)
            {
                IDXGIFactory1* factory = nullptr;
                if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                                   reinterpret_cast<void**>(&factory))))
                {
                    UINT adapter_count = 0;
                    IDXGIAdapter1* adapter = nullptr;
                    while (factory->EnumAdapters1(adapter_count, &adapter) != DXGI_ERROR_NOT_FOUND)
                    {
                        adapter->Release();
                        adapter_count++;
                    }
                    factory->Release();

                    if (adapter_count >= 2)
                    {
                        result = DisplayCaptureMethod::Wgc;
                    }
                }
            }
        }

        return result;
    }

    return method_;
}

// ========== 释放采集资源（保留设备） ==========

void MonitorCapture::FreeCaptureData()
{
    wgc_.Shutdown();
    dxgi_.Shutdown();
    gdi_.Free();

    width_ = 0;
    height_ = 0;
    rotation_ = 0;
    monitor_x_ = 0;
    monitor_y_ = 0;
    reset_timeout_ = 0.0f;
}

// ========== 重新查找显示器句柄 ==========

void MonitorCapture::UpdateMonitorHandle()
{
    MonitorInfo info;
    if (FindMonitorById(monitor_id_, info))
    {
        handle_ = info.handle;
    }
}

// ========== 初始化 ==========

bool MonitorCapture::Init(ID3D11Device* device, const char* monitor_id,
                          DisplayCaptureMethod method, bool capture_cursor, bool force_sdr)
{
    Shutdown();

    device_ = device;
    method_ = method;
    capture_cursor_ = capture_cursor;
    force_sdr_ = force_sdr;

    strcpy_s(monitor_id_, _countof(monitor_id_), monitor_id);

    // ---- 查找显示器句柄 ----
    MonitorInfo info;
    if (FindMonitorById(monitor_id_, info))
    {
        handle_ = info.handle;
    }
    else
    {
        return false;
    }

    // ---- 选择采集方法 ----
    DisplayCaptureMethod chosen = ChooseMethod(handle_);
    if (use_gdi_)
    {
        // ---- GDI 回退路线 ----
        uint32_t w = info.rect.right - info.rect.left;
        uint32_t h = info.rect.bottom - info.rect.top;
        gdi_.Init(info.rect.left, info.rect.top, w, h, capture_cursor_);
        width_ = w;
        height_ = h;
    }

    reset_timeout_ = RESET_INTERVAL_SEC;

    // ---- 打印实际使用的采集方法 ----
    const char* method_name = "unknown";
    if (use_gdi_)
    {
        method_name = "GDI";
    }
    else
    {
        DisplayCaptureMethod chosen = ChooseMethod(handle_);
        switch (chosen)
        {
        case DisplayCaptureMethod::Dxgi: method_name = "DXGI"; break;
        case DisplayCaptureMethod::Wgc: method_name = "WGC"; break;
        case DisplayCaptureMethod::Auto: method_name = "Auto"; break;
        }
    }
    qDebug("[MonitorCapture] 采集方法: %s", method_name);

    return true;
}

// ========== 停止采集并释放资源 ==========

void MonitorCapture::Shutdown()
{
    FreeCaptureData();
    device_ = nullptr;
    handle_ = nullptr;
    showing_ = false;
    use_gdi_ = false;
}

// ========== 每帧调用 ==========

void MonitorCapture::Tick(float delta_seconds)
{
    // ---- GDI 路线：直接 BitBlt 采集 ----
    if (use_gdi_)
    {
        gdi_.Capture(nullptr);
        return;
    }

    // ---- 没有显示器句柄时尝试重新查找 ----
    if (!handle_)
    {
        UpdateMonitorHandle();
        if (!handle_)
        {
            return;
        }
    }

    DisplayCaptureMethod chosen = ChooseMethod(handle_);

    // ---- WGC 路线 ----
    if (chosen == DisplayCaptureMethod::Wgc)
    {
        // ---- 需要重置 WGC 时先释放 ----
        if (reset_wgc_ && wgc_.IsActive())
        {
            wgc_.Shutdown();
            reset_wgc_ = false;
            reset_timeout_ = RESET_INTERVAL_SEC;
        }

        // ---- 没有活跃的 WGC 实例 → 等待 3 秒重试 ----
        if (!wgc_.IsActive())
        {
            reset_timeout_ += delta_seconds;

            if (reset_timeout_ >= RESET_INTERVAL_SEC)
            {
                // ---- 尝试初始化 WGC ----
                if (!wgc_.InitMonitor(device_, handle_, capture_cursor_, force_sdr_))
                {
                    // ---- 第一次失败后重新查找显示器句柄再试 ----
                    UpdateMonitorHandle();
                    if (handle_)
                    {
                        wgc_.InitMonitor(device_, handle_, capture_cursor_, force_sdr_);
                    }
                }

                reset_timeout_ = 0.0f;
            }
        }

        // ---- 有活跃实例时检查帧 ----
        if (wgc_.IsActive())
        {
            wgc_.Tick();
            width_ = wgc_.Width();
            height_ = wgc_.Height();
        }
    }
    else
    {
        // ---- DXGI 路线 ----
        // 如果 WGC 实例还在，先释放
        if (wgc_.IsActive())
        {
            wgc_.Shutdown();
        }

        if (!dxgi_.IsActive())
        {
            reset_timeout_ += delta_seconds;

            if (reset_timeout_ >= RESET_INTERVAL_SEC)
            {
                // ---- 尝试初始化 DXGI ----
                int dxgi_index = DxgiDuplicator::GetMonitorIndex(handle_);
                if (dxgi_index == -1)
                {
                    UpdateMonitorHandle();
                    if (handle_)
                    {
                        dxgi_.Init(device_, handle_);
                    }
                }
                else
                {
                    dxgi_.Init(device_, handle_);
                }

                reset_timeout_ = 0.0f;
            }
        }

        // ---- 有活跃实例时更新帧 ----
        if (dxgi_.IsActive())
        {
            // ---- 采集光标（DXGI 路线需要外部光标合成） ----
            if (capture_cursor_)
            {
                cursor_.Capture();
            }

            // ---- 更新帧，失败时释放资源等待重试 ----
            if (!dxgi_.UpdateFrame())
            {
                FreeCaptureData();
            }
            else if (width_ == 0)
            {
                // ---- 首次成功 → 读取尺寸和偏移 ----
                width_ = dxgi_.Width();
                height_ = dxgi_.Height();
                rotation_ = dxgi_.Rotation();
                monitor_x_ = dxgi_.MonitorX();
                monitor_y_ = dxgi_.MonitorY();
            }
        }
    }

    showing_ = true;
}

// ========== 获取当前帧 ==========

bool MonitorCapture::GetFrame(CaptureFrame& out_frame)
{
    if (use_gdi_)
    {
        // ---- GDI 路线：返回 CPU 内存数据 ----
        const uint8_t* data = nullptr;
        uint32_t stride = 0;
        if (gdi_.GetFrame(data, stride))
        {
            out_frame.gpu_texture = nullptr;
            out_frame.cpu_data = data;
            out_frame.cpu_stride = stride;
            out_frame.width = gdi_.Width();
            out_frame.height = gdi_.Height();
            out_frame.rotation = 0;
            return true;
        }
        return false;
    }

    if (method_ == DisplayCaptureMethod::Wgc || wgc_.IsActive())
    {
        // ---- WGC 路线：返回 GPU 纹理 ----
        ID3D11Texture2D* tex = wgc_.GetTexture();
        if (tex)
        {
            out_frame.gpu_texture = tex;
            out_frame.cpu_data = nullptr;
            out_frame.width = wgc_.Width();
            out_frame.height = wgc_.Height();
            out_frame.rotation = 0;
            return true;
        }
        return false;
    }

    // ---- DXGI 路线：返回 GPU 纹理 ----
    ID3D11Texture2D* tex = dxgi_.GetTexture();
    if (tex)
    {
        out_frame.gpu_texture = tex;
        out_frame.cpu_data = nullptr;
        out_frame.width = dxgi_.Width();
        out_frame.height = dxgi_.Height();
        out_frame.rotation = dxgi_.Rotation();
        return true;
    }

    return false;
}

// ========== 获取光标信息 ==========

bool MonitorCapture::GetCursorInfo(CursorInfo& out_info)
{
    // ---- WGC 路线光标由内部处理，GDI 路线光标已叠加到画面 ----
    // ---- 仅 DXGI 路线需要外部光标合成 ----
    if (use_gdi_ || wgc_.IsActive())
    {
        return false;
    }

    if (!capture_cursor_)
    {
        return false;
    }

    return cursor_.GetInfo(out_info);
}

// ========== 查询方法 ==========

uint32_t MonitorCapture::Width() const
{
    if (use_gdi_)
    {
        return gdi_.Width();
    }
    if (wgc_.IsActive())
    {
        return wgc_.Width();
    }
    // ---- DXGI 路线考虑旋转：90/270 度时宽高互换 ----
    if (rotation_ % 180 == 0)
    {
        return width_;
    }
    return height_;
}

uint32_t MonitorCapture::Height() const
{
    if (use_gdi_)
    {
        return gdi_.Height();
    }
    if (wgc_.IsActive())
    {
        return wgc_.Height();
    }
    if (rotation_ % 180 == 0)
    {
        return height_;
    }
    return width_;
}

DisplayCaptureMethod MonitorCapture::Method() const
{
    return method_;
}

bool MonitorCapture::IsActive() const
{
    if (use_gdi_)
    {
        return gdi_.IsValid();
    }
    return dxgi_.IsActive() || wgc_.IsActive();
}
