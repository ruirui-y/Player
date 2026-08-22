#include "MonitorCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstring>
#include <QDebug>
#include "Common/LogManager.h"
#include "DxgiDuplicator.h"
#include "WgcCapture.h"

// ========== 辅助：采集方法名（日志用） ==========
static const char* MethodName(DisplayCaptureMethod method);

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
    // ---- 没有 D3D11 设备 → GDI 回退（唯一不依赖 GPU 的路线）----
    if (!device_)
        return DisplayCaptureMethod::Gdi;

    // ---- WGC 不支持 → 强制 DXGI ----
    if (!WgcCapture::IsSupported())
        return DisplayCaptureMethod::Dxgi;

    // ---- 用户显式指定（非 Auto）→ 直接用 ----
    if (method_ != DisplayCaptureMethod::Auto)
        return method_;

    // ---- Auto：默认 DXGI，按环境升级 WGC ----
    // DXGI 拿不到显示器索引 → 切换 WGC
    if (DxgiDuplicator::GetMonitorIndex(monitor) == -1)
        return DisplayCaptureMethod::Wgc;

    // 笔记本电池 + 双显卡 → 切换 WGC（省电/兼容）
    if (IsLaptopDualGpu())
        return DisplayCaptureMethod::Wgc;

    return DisplayCaptureMethod::Dxgi;
}

// ========== 笔记本电池 + 双显卡判定 ==========

bool MonitorCapture::IsLaptopDualGpu() const
{
    SYSTEM_POWER_STATUS status;
    if (!GetSystemPowerStatus(&status) || status.BatteryFlag >= 128)
        return false;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory))))
        return false;

    UINT adapter_count = 0;
    IDXGIAdapter1* adapter = nullptr;
    while (factory->EnumAdapters1(adapter_count, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        adapter->Release();
        adapter_count++;
    }
    factory->Release();

    return adapter_count >= 2;
}

// ========== 释放采集资源（保留设备） ==========

void MonitorCapture::FreeCaptureData()
{
    if (backend_)
    {
        backend_->Shutdown();
        backend_.reset();
    }

    width_ = 0;
    height_ = 0;
    rotation_ = 0;
    next_retry_ = {};
}

// ========== 重新查找显示器句柄 ==========

void MonitorCapture::UpdateMonitorHandle()
{
    MonitorInfo info;
    if (FindMonitorById(monitor_id_, info))
    {
        handle_ = info.handle;
        rect_ = info.rect;
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
    if (!FindMonitorById(monitor_id_, info))
    {
        return false;
    }
    handle_ = info.handle;
    rect_ = info.rect;

    // ---- 解析实际生效方法（后端在首次 Capture 时懒创建）----
    active_method_ = ChooseMethod(handle_);

    // ---- 打印实际使用的采集方法 ----
    qDebug("[MonitorCapture] 采集方法: %s", MethodName(active_method_));

    return true;
}

// ========== 停止采集并释放资源 ==========

void MonitorCapture::Shutdown()
{
    FreeCaptureData();
    device_ = nullptr;
    handle_ = nullptr;
    rect_ = {0, 0, 0, 0};
    showing_ = false;
    active_method_ = DisplayCaptureMethod::Auto;
}

// ========== 采集方法名（日志用） ==========

static const char* MethodName(DisplayCaptureMethod method)
{
    switch (method)
    {
    case DisplayCaptureMethod::Gdi:  return "GDI";
    case DisplayCaptureMethod::Dxgi: return "DXGI";
    case DisplayCaptureMethod::Wgc:  return "WGC";
    case DisplayCaptureMethod::Auto: return "Auto";
    }
    return "unknown";
}

// ========== 确保后端就绪（创建/切换 + 3 秒退避重建） ==========

void MonitorCapture::EnsureBackend()
{
    // ---- 方法变化或后端缺失 → 切换后端 ----
    if (!backend_ || backend_->Kind() != active_method_)
    {
        backend_ = CreateCaptureBackend(active_method_);
        next_retry_ = {};
        if (!backend_)
        {
            return;
        }
    }

    // ---- 同步初始化参数（handle/rect 可能已被刷新） ----
    ctx_.device = device_;
    ctx_.monitor = handle_;
    ctx_.rect = rect_;
    ctx_.capture_cursor = capture_cursor_;
    ctx_.force_sdr = force_sdr_;

    // ---- 后端可用或未到退避时间 → 直接返回 ----
    auto now = std::chrono::steady_clock::now();
    if (backend_->IsActive() || now < next_retry_)
    {
        return;
    }

    // ---- 退避到期 → 尝试初始化；失败后重新查找句柄再试一次 ----
    if (backend_->Init(ctx_))
    {
        next_retry_ = {};
        return;
    }

    UpdateMonitorHandle();
    ctx_.monitor = handle_;
    if (backend_->Init(ctx_))
    {
        next_retry_ = {};
        return;
    }

    LogManager::Log("WARN", "[MonitorCapture] %s 初始化失败，3 秒后重试", MethodName(active_method_));
    next_retry_ = now + std::chrono::seconds(static_cast<long>(RESET_INTERVAL_SEC));
}

// ========== 采集一帧（原 Tick + GetFrame 合并） ==========

bool MonitorCapture::Capture(CaptureFrame& out_frame)
{
    // ---- 没有显示器句柄时尝试重新查找 ----
    if (!handle_)
    {
        UpdateMonitorHandle();
        if (!handle_)
        {
            return false;
        }
    }

    // ---- 解析当前生效方法（Auto 可随环境在 Dxgi/Wgc 间切换）----
    active_method_ = ChooseMethod(handle_);

    // ---- 确保后端就绪 ----
    EnsureBackend();
    if (!backend_ || !backend_->IsActive())
    {
        return false;
    }

    // ---- 采集一帧 ----
    if (!backend_->AcquireFrame())
    {
        // ---- 后端刚死亡（如 DXGI ACCESS_LOST）→ 下一帧立即重建一次，失败再进 3 秒退避 ----
        if (!backend_->IsActive() && next_retry_ == std::chrono::steady_clock::time_point{})
        {
            next_retry_ = std::chrono::steady_clock::now();
        }
        return false;
    }

    // ---- 刷新几何信息（防 0 值覆盖） ----
    uint32_t frame_width = backend_->Width();
    if (frame_width > 0)
    {
        width_ = frame_width;
        height_ = backend_->Height();
        rotation_ = backend_->Rotation();
    }

    showing_ = true;
    bool ok = backend_->GetFrame(out_frame);
    return ok;
}

// ========== 获取光标信息 ==========

bool MonitorCapture::GetCursorInfo(CursorInfo& out_info)
{
    // ---- WGC 路线光标由内部处理，GDI 路线光标已叠加到画面 ----
    // ---- 仅 DXGI 路线需要外部光标合成 ----
    if (!backend_ || backend_->Kind() != DisplayCaptureMethod::Dxgi)
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
    // ---- 考虑旋转：90/270 度时宽高互换（仅 DXGI 有非 0 旋转） ----
    if (rotation_ % 180 == 0)
    {
        return width_;
    }
    return height_;
}

uint32_t MonitorCapture::Height() const
{
    if (rotation_ % 180 == 0)
    {
        return height_;
    }
    return width_;
}

DisplayCaptureMethod MonitorCapture::Method() const
{
    return active_method_;
}

bool MonitorCapture::IsActive() const
{
    return backend_ && backend_->IsActive();
}
