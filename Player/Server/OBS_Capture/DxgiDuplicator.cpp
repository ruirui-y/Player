#include "DxgiDuplicator.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <QDebug>
#include "Common/LogManager.h"

// ========== 静态方法：将 HMONITOR 映射到 DXGI output 索引 ==========

int DxgiDuplicator::GetMonitorIndex(HMONITOR monitor)
{
    if (!monitor)
    {
        return -1;
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    {
        return -1;
    }

    int result = -1;
    IDXGIAdapter1* adapter = nullptr;

    // ---- 遍历所有适配器，找到对应的 Output ----
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai)
    {
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi)
        {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)))
            {
                if (desc.Monitor == monitor)
                {
                    result = static_cast<int>(oi);
                    // ---- 同时记录适配器索引？不，OBS 用的是全局 output 索引 ----
                    // 实际上 gs_duplicator 的 index 是全局 output 索引
                    // 这里简化为返回 output 在该适配器上的索引
                    // 调用方需要同时知道适配器索引
                    output->Release();
                    adapter->Release();
                    factory->Release();
                    return result;
                }
            }
            output->Release();
        }
        adapter->Release();
    }

    factory->Release();
    return -1;
}

// ========== 创建 IDXGIOutputDuplication ==========

bool DxgiDuplicator::CreateDuplication()
{
    if (!device_ || !monitor_)
    {
        return false;
    }

    // ---- 获取 DXGI Factory ----
    IDXGIDevice* dxgi_device = nullptr;
    if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))))
    {
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    if (FAILED(dxgi_device->GetAdapter(&adapter)))
    {
        dxgi_device->Release();
        return false;
    }
    dxgi_device->Release();

    // ---- 遍历 Output 找到目标显示器 ----
    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;
    bool found = false;

    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_OUTPUT_DESC desc;
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor_)
        {
            monitor_index_ = static_cast<int>(i);

            // ---- 获取 DXGI_OUTPUT_DESC 中的桌面坐标和旋转 ----
            monitor_x_ = desc.DesktopCoordinates.left;
            monitor_y_ = desc.DesktopCoordinates.top;

            switch (desc.Rotation)
            {
            case DXGI_MODE_ROTATION_IDENTITY:
                rotation_ = 0;
                break;
            case DXGI_MODE_ROTATION_ROTATE90:
                rotation_ = 90;
                break;
            case DXGI_MODE_ROTATION_ROTATE180:
                rotation_ = 180;
                break;
            case DXGI_MODE_ROTATION_ROTATE270:
                rotation_ = 270;
                break;
            }

            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1),
                                                  reinterpret_cast<void**>(&output1))))
            {
                found = true;
            }
            output->Release();
            break;
        }
        output->Release();
    }

    adapter->Release();

    if (!found || !output1)
    {
        return false;
    }

    // ---- 创建 Desktop Duplication ----
    // 注意：DuplicateOutput 需要 D3D11 设备在正确的适配器上创建
    HRESULT hr = output1->DuplicateOutput(device_, &duplication_);
    output1->Release();

    if (FAILED(hr))
    {
        // DXGI_ERROR_ACCESS_DENIED: 另一个进程已经占用了 Desktop Duplication
        return false;
    }

    active_ = true;
    return true;
}

// ========== 释放当前帧纹理 ==========

void DxgiDuplicator::ReleaseFrame()
{
    // ---- 释放私有目标纹理 ----
    if (target_texture_)
    {
        target_texture_->Release();
        target_texture_ = nullptr;
    }
    target_width_ = 0;
    target_height_ = 0;
}

// ========== 构造 / 析构 ==========

DxgiDuplicator::DxgiDuplicator()
{
}

DxgiDuplicator::~DxgiDuplicator()
{
    Shutdown();
}

// ========== 初始化 ==========

bool DxgiDuplicator::Init(ID3D11Device* device, HMONITOR monitor)
{
    Shutdown();

    if (!device || !monitor)
    {
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&context_);
    monitor_ = monitor;

    return CreateDuplication();
}

// ========== 初始化（CaptureBackend 接口） ==========

bool DxgiDuplicator::Init(const BackendContext& ctx)
{
    return Init(ctx.device, ctx.monitor);
}

// ========== 释放 ==========

void DxgiDuplicator::Shutdown()
{
    ReleaseFrame();

    if (duplication_)
    {
        duplication_->Release();
        duplication_ = nullptr;
    }

    if (context_)
    {
        context_->Release();
        context_ = nullptr;
    }

    device_ = nullptr;
    monitor_ = nullptr;
    monitor_index_ = -1;
    width_ = 0;
    height_ = 0;
    rotation_ = 0;
    monitor_x_ = 0;
    monitor_y_ = 0;
    active_ = false;
}

// ========== 更新帧 ==========

bool DxgiDuplicator::UpdateFrame(uint32_t timeout_ms)
{
    if (!active_ || !duplication_)
    {
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info = {0};
    IDXGIResource* resource = nullptr;

    // ---- 获取下一帧（不提前释放上一帧，timeout 时保留旧帧） ----
    HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &frame_info, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // ---- 超时是正常的，桌面没有变化 → 保留上一帧纹理 ----
        // 但如果是重建后首帧（target_texture_ 为空），用更大超时再试一次
        if (!target_texture_)
        {
            LogManager::Log("DBG", "[DxgiDuplicator] 重建后首帧，等待桌面变化...");
            hr = duplication_->AcquireNextFrame(500, &frame_info, &resource);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                return true;                                            // 仍然没有变化，等待下次 Tick
            // 获取到了帧或出错 → 继续往下处理
        }
        else
        {
            return true;                                                // 保留上一帧
        }
    }

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        // ---- 访问丢失 → 需要重建 duplication ----
        LogManager::Log("WARN", "[DxgiDuplicator] DXGI_ERROR_ACCESS_LOST，需要重建");
        active_ = false;
        ReleaseFrame();
        return false;
    }

    if (FAILED(hr) || !resource)
    {
        LogManager::Log("WARN", "[DxgiDuplicator] AcquireNextFrame 失败, HR=0x%08X",
                        (unsigned)hr);
        ReleaseFrame();
        return false;
    }

    // ---- 从 IDXGIResource 获取 D3D11 纹理 ----
    ID3D11Texture2D* desktop_texture = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                   reinterpret_cast<void**>(&desktop_texture));
    resource->Release();

    if (FAILED(hr) || !desktop_texture)
    {
        duplication_->ReleaseFrame();
        return false;
    }

    // ---- 读取桌面纹理尺寸和格式 ----
    D3D11_TEXTURE2D_DESC desc;
    desktop_texture->GetDesc(&desc);

    // ---- 创建或重建 target_texture_（尺寸变化时） ----
    // DXGI Desktop Duplication 的纹理在 ReleaseFrame 后内容失效，
    // 必须在 ReleaseFrame 前拷贝到私有纹理（与 OBS gs_duplicator 一致）
    if (!target_texture_ || target_width_ != desc.Width || target_height_ != desc.Height)
    {
        if (target_texture_)
        {
            target_texture_->Release();
            target_texture_ = nullptr;
        }

        D3D11_TEXTURE2D_DESC target_desc = desc;
        target_desc.Usage = D3D11_USAGE_DEFAULT;
        target_desc.BindFlags = 0;
        target_desc.CPUAccessFlags = 0;
        target_desc.MiscFlags = 0;

        hr = device_->CreateTexture2D(&target_desc, nullptr, &target_texture_);
        if (FAILED(hr))
        {
            qDebug() << "[DxgiDuplicator] 创建 target 纹理失败, HR =" << hr;
            desktop_texture->Release();
            duplication_->ReleaseFrame();
            return false;
        }

        target_width_ = desc.Width;
        target_height_ = desc.Height;

        qDebug() << "[DxgiDuplicator] 创建 target 纹理:"
                 << desc.Width << "x" << desc.Height;
    }

    // ---- 在 ReleaseFrame 前拷贝到私有纹理（关键！） ----
    context_->CopyResource(target_texture_, desktop_texture);

    // ---- 释放桌面纹理并 ReleaseFrame ----
    desktop_texture->Release();
    duplication_->ReleaseFrame();

    // ---- 更新尺寸 ----
    width_ = desc.Width;
    height_ = desc.Height;

    // ---- 首帧时打印纹理格式（诊断黑屏：格式不匹配会导致 CopyResource 静默失败） ----
    static bool format_logged = false;
    if (!format_logged)
    {
        const char* fmt_name = "unknown";
        switch (desc.Format)
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM: fmt_name = "BGRA"; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: fmt_name = "RGBA"; break;
        case DXGI_FORMAT_R10G10B10A2_UNORM: fmt_name = "R10G10B10A2"; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: fmt_name = "R16G16B16A16_FLOAT"; break;
        default: break;
        }
        qDebug("[DxgiDuplicator] 首帧纹理格式: %s, %ux%u, MipLevels=%u, ArraySize=%u",
               fmt_name, desc.Width, desc.Height, desc.MipLevels, desc.ArraySize);
        format_logged = true;
    }

    return true;
}

// ========== 获取当前帧纹理 ==========

ID3D11Texture2D* DxgiDuplicator::GetTexture() const
{
    return target_texture_;
}

// ========== 查询方法 ==========

uint32_t DxgiDuplicator::Width() const
{
    return width_;
}

uint32_t DxgiDuplicator::Height() const
{
    return height_;
}

int DxgiDuplicator::Rotation() const
{
    return rotation_;
}

int DxgiDuplicator::MonitorX() const
{
    return monitor_x_;
}

int DxgiDuplicator::MonitorY() const
{
    return monitor_y_;
}

bool DxgiDuplicator::IsActive() const
{
    return active_;
}

// ========== 采集一帧（CaptureBackend 接口） ==========

bool DxgiDuplicator::AcquireFrame()
{
    // ---- UpdateFrame 返回 false 表示真错误（ACCESS_LOST 等） → 触发重建 ----
    // ---- 超时/桌面无变化返回 true（保留上一帧），不视为失败 ----
    return UpdateFrame();
}

// ========== 获取当前帧（CaptureBackend 接口） ==========

bool DxgiDuplicator::GetFrame(CaptureFrame& out)
{
    ID3D11Texture2D* tex = GetTexture();
    if (!tex)
    {
        return false;
    }

    out.gpu_texture = tex;
    out.cpu_data = nullptr;
    out.width = width_;
    out.height = height_;
    out.rotation = rotation_;
    return true;
}
