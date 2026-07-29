#include "WgcCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <roapi.h>
#include <wrl.h>

#pragma comment(lib, "runtimeobject.lib")

// ========== 辅助：HSTRING 管理 ==========

class ScopedHString
{
public:
    explicit ScopedHString(const wchar_t* str)                          // 构造并创建 HSTRING
    {
        WindowsCreateString(str, static_cast<UINT32>(wcslen(str)), &hs_);
    }
    ~ScopedHString()                                                    // 析构并销毁 HSTRING
    {
        if (hs_)
        {
            WindowsDeleteString(hs_);
        }
    }
    HSTRING Get() const { return hs_; }                                // 获取 HSTRING

private:
    HSTRING hs_{nullptr};
};

// ========== FrameArrived 回调实现 ==========

// SDK 头文件已特化 ITypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*>
// 注意：第二个参数是 IInspectable*（FrameArrived 不传帧，需调 TryGetNextFrame 获取）
using FramePoolHandler = ABI::Windows::Foundation::ITypedEventHandler<
    ABI::Windows::Graphics::Capture::Direct3D11CaptureFramePool*,
    IInspectable*>;

class FrameArrivedHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          FramePoolHandler,
          Microsoft::WRL::FtmBase>
{
public:
    explicit FrameArrivedHandler(WgcCapture* owner)                     // 构造，绑定 owner 指针
        : owner_(owner)
    {
    }

    // FrameArrived 事件触发时调用
    HRESULT STDMETHODCALLTYPE Invoke(
        ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool* /*sender*/,
        IInspectable* /*args*/) override
    {
        // ---- 仅设置标志，帧获取延迟到主线程 Tick 中执行 ----
        if (owner_)
        {
            owner_->OnFrameArrived();
        }
        return S_OK;
    }

private:
    WgcCapture* owner_;                                                 // WgcCapture 实例指针
};

// ========== 辅助：CaptureItem 关闭回调 ==========

using ClosedHandler = ABI::Windows::Foundation::ITypedEventHandler<
    ABI::Windows::Graphics::Capture::GraphicsCaptureItem*,
    IInspectable*>;

class CaptureClosedHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ClosedHandler,
          Microsoft::WRL::FtmBase>
{
public:
    explicit CaptureClosedHandler(std::atomic<bool>* closed_flag)
        : closed_flag_(closed_flag)
    {
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        ABI::Windows::Graphics::Capture::IGraphicsCaptureItem* /*sender*/,
        IInspectable* /*args*/) override
    {
        if (closed_flag_)
        {
            closed_flag_->store(true);
        }
        return S_OK;
    }

private:
    std::atomic<bool>* closed_flag_;                                    // 指向 WgcCapture::closed_ 的指针
};

// ========== 构造 / 析构 ==========

WgcCapture::WgcCapture()
{
}

WgcCapture::~WgcCapture()
{
    Shutdown();
}

// ========== 静态方法：检测 WGC 是否支持 ==========

bool WgcCapture::IsSupported()
{
    // ---- 尝试获取 IGraphicsCaptureItemInterop 的 Activation Factory ----
    ScopedHString class_id(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);
    IGraphicsCaptureItemInterop* interop = nullptr;

    HRESULT hr = RoGetActivationFactory(class_id.Get(),
                                         __uuidof(IGraphicsCaptureItemInterop),
                                         reinterpret_cast<void**>(&interop));

    if (SUCCEEDED(hr) && interop)
    {
        interop->Release();
        return true;
    }

    return false;
}

// ========== 从 ID3D11Device 创建 WinRT IDirect3DDevice ==========

bool WgcCapture::CreateDirect3DDeviceFromD3D11(ID3D11Device* d3d11_device)
{
    if (!d3d11_device)
    {
        return false;
    }

    // ---- 获取 IDXGIDevice ----
    HRESULT hr = d3d11_device->QueryInterface(__uuidof(IDXGIDevice),
                                                reinterpret_cast<void**>(&dxgi_device_));
    if (FAILED(hr))
    {
        return false;
    }

    // ---- 动态加载 CreateDirect3D11DeviceFromDXGIDevice ----
    HMODULE module = LoadLibraryW(L"windows.graphics.directx.direct3d11.dll");
    if (!module)
    {
        return false;
    }

    typedef HRESULT(WINAPI* PFN_CreateDirect3D11DeviceFromDXGIDevice)(
        IDXGIDevice*, IInspectable**);

    auto pfn = reinterpret_cast<PFN_CreateDirect3D11DeviceFromDXGIDevice>(
        GetProcAddress(module, "CreateDirect3D11DeviceFromDXGIDevice"));

    if (!pfn)
    {
        FreeLibrary(module);
        return false;
    }

    // ---- 调用函数获取 IInspectable，再 QI 为 IDirect3DDevice ----
    IInspectable* inspectable = nullptr;
    hr = pfn(dxgi_device_, &inspectable);
    FreeLibrary(module);

    if (FAILED(hr) || !inspectable)
    {
        return false;
    }

    hr = inspectable->QueryInterface(
        __uuidof(ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice),
        reinterpret_cast<void**>(&direct3d_device_));

    inspectable->Release();

    return SUCCEEDED(hr);
}

// ========== 从 HMONITOR 创建 GraphicsCaptureItem ==========

bool WgcCapture::CreateItemForMonitor(HMONITOR monitor)
{
    // ---- 获取 IGraphicsCaptureItemInterop ----
    ScopedHString class_id(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);
    IGraphicsCaptureItemInterop* interop = nullptr;

    HRESULT hr = RoGetActivationFactory(class_id.Get(),
                                         __uuidof(IGraphicsCaptureItemInterop),
                                         reinterpret_cast<void**>(&interop));
    if (FAILED(hr) || !interop)
    {
        return false;
    }

    // ---- 通过 Interop 从 HMONITOR 创建 CaptureItem ----
    hr = interop->CreateForMonitor(monitor,
                                    __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
                                    reinterpret_cast<void**>(&item_));
    interop->Release();

    return SUCCEEDED(hr) && item_ != nullptr;
}

// ========== 从 HWND 创建 GraphicsCaptureItem ==========

bool WgcCapture::CreateItemForWindow(HWND window)
{
    ScopedHString class_id(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);
    IGraphicsCaptureItemInterop* interop = nullptr;

    HRESULT hr = RoGetActivationFactory(class_id.Get(),
                                         __uuidof(IGraphicsCaptureItemInterop),
                                         reinterpret_cast<void**>(&interop));
    if (FAILED(hr) || !interop)
    {
        return false;
    }

    // ---- 通过 Interop 从 HWND 创建 CaptureItem ----
    hr = interop->CreateForWindow(window,
                                   __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
                                   reinterpret_cast<void**>(&item_));
    interop->Release();

    return SUCCEEDED(hr) && item_ != nullptr;
}

// ========== 创建 FramePool 和 CaptureSession 并启动 ==========

bool WgcCapture::StartCapture()
{
    if (!direct3d_device_ || !item_)
    {
        return false;
    }

    // ---- 获取 CaptureItem 尺寸 ----
    ABI::Windows::Graphics::SizeInt32 size;
    item_->get_Size(&size);

    width_ = static_cast<uint32_t>(size.Width);
    height_ = static_cast<uint32_t>(size.Height);

    // ---- 获取 Direct3D11CaptureFramePool 静态工厂 ----
    ScopedHString pool_class_id(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool);
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics* pool_stats = nullptr;

    HRESULT hr = RoGetActivationFactory(pool_class_id.Get(),
                                         __uuidof(ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics),
                                         reinterpret_cast<void**>(&pool_stats));
    if (FAILED(hr) || !pool_stats)
    {
        return false;
    }

    // ---- 创建 FramePool（2 个缓冲区，BGRA 格式） ----
    hr = pool_stats->Create(direct3d_device_,
                             ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                             2,
                             size,
                             &frame_pool_);
    pool_stats->Release();

    if (FAILED(hr) || !frame_pool_)
    {
        return false;
    }

    // ---- 注册 FrameArrived 事件 ----
    auto handler = Microsoft::WRL::Make<FrameArrivedHandler>(this);
    // FrameArrivedHandler 通过 FramePoolHandler 和 FtmBase 两条路径继承 IUnknown（菱形继承），
    // ComPtr 隐式转换会因基类二义性失败，改用 As() 走 QueryInterface
    handler.As(&frame_arrived_handler_);

    hr = frame_pool_->add_FrameArrived(handler.Get(), &frame_arrived_token_);
    if (FAILED(hr))
    {
        return false;
    }

    // ---- 注册 CaptureItem Closed 事件 ----
    auto closed_handler = Microsoft::WRL::Make<CaptureClosedHandler>(&closed_);
    hr = item_->add_Closed(closed_handler.Get(), &closed_token_);
    if (FAILED(hr))
    {
        // Closed 事件注册失败不影响核心功能
    }

    // ---- 创建 CaptureSession ----
    hr = frame_pool_->CreateCaptureSession(item_, &session_);
    if (FAILED(hr) || !session_)
    {
        return false;
    }

    // ---- QI 获取 IGraphicsCaptureSession2（put_IsCursorCaptureEnabled 在此接口） ----
    session_->QueryInterface(__uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2),
                              reinterpret_cast<void**>(&session2_));

    // ---- 配置光标采集 ----
    if (session2_)
    {
        session2_->put_IsCursorCaptureEnabled(cursor_ ? TRUE : FALSE);
    }

    // ---- 启动采集 ----
    hr = session_->StartCapture();
    if (FAILED(hr))
    {
        return false;
    }

    active_ = true;
    return true;
}

// ========== 释放帧纹理 ==========

void WgcCapture::ReleaseFrame()
{
    std::lock_guard<std::mutex> lock(texture_mutex_);

    if (current_texture_)
    {
        current_texture_->Release();
        current_texture_ = nullptr;
    }
}

// ========== 显示器采集初始化 ==========

bool WgcCapture::InitMonitor(ID3D11Device* device, HMONITOR monitor, bool cursor, bool force_sdr)
{
    Shutdown();

    if (!device || !monitor)
    {
        return false;
    }

    device_ = device;
    cursor_ = cursor;
    force_sdr_ = force_sdr;

    // ---- 第一步：创建 WinRT Direct3D 设备 ----
    if (!CreateDirect3DDeviceFromD3D11(device_))
    {
        Shutdown();
        return false;
    }

    // ---- 第二步：创建 GraphicsCaptureItem ----
    if (!CreateItemForMonitor(monitor))
    {
        Shutdown();
        return false;
    }

    // ---- 第三步：创建 FramePool + Session 并启动 ----
    if (!StartCapture())
    {
        Shutdown();
        return false;
    }

    return true;
}

// ========== 窗口采集初始化 ==========

bool WgcCapture::InitWindow(ID3D11Device* device, HWND window, bool client_area, bool cursor, bool force_sdr)
{
    Shutdown();

    if (!device || !window)
    {
        return false;
    }

    device_ = device;
    cursor_ = cursor;
    force_sdr_ = force_sdr;
    client_area_ = client_area;

    // ---- 第一步：创建 WinRT Direct3D 设备 ----
    if (!CreateDirect3DDeviceFromD3D11(device_))
    {
        Shutdown();
        return false;
    }

    // ---- 第二步：创建 GraphicsCaptureItem ----
    if (!CreateItemForWindow(window))
    {
        Shutdown();
        return false;
    }

    // ---- 第三步：创建 FramePool + Session 并启动 ----
    if (!StartCapture())
    {
        Shutdown();
        return false;
    }

    return true;
}

// ========== 停止采集并释放资源 ==========

void WgcCapture::Shutdown()
{
    ReleaseFrame();

    // ---- 移除事件回调 ----
    if (frame_pool_)
    {
        frame_pool_->remove_FrameArrived(frame_arrived_token_);
        frame_pool_->Release();
        frame_pool_ = nullptr;
    }

    if (item_)
    {
        item_->remove_Closed(closed_token_);
        item_->Release();
        item_ = nullptr;
    }

    // ---- 通过 IClosable 关闭 session ----
    if (session_)
    {
        ABI::Windows::Foundation::IClosable* closable = nullptr;
        if (SUCCEEDED(session_->QueryInterface(__uuidof(ABI::Windows::Foundation::IClosable),
                                                 reinterpret_cast<void**>(&closable))))
        {
            closable->Close();
            closable->Release();
        }
        session_->Release();
        session_ = nullptr;
    }

    if (session2_)
    {
        session2_->Release();
        session2_ = nullptr;
    }

    if (direct3d_device_)
    {
        direct3d_device_->Release();
        direct3d_device_ = nullptr;
    }

    if (dxgi_device_)
    {
        dxgi_device_->Release();
        dxgi_device_ = nullptr;
    }

    frame_arrived_handler_.Reset();

    device_ = nullptr;
    width_ = 0;
    height_ = 0;
    active_ = false;
    new_frame_arrived_ = false;
    closed_ = false;
}

// ========== FrameArrived 事件回调 ==========

void WgcCapture::OnFrameArrived()
{
    // ---- 仅设置标志，帧获取在主线程 Tick 中执行 ----
    new_frame_arrived_.store(true);
}

// ========== 每帧调用，检查是否有新帧到达 ==========

bool WgcCapture::Tick()
{
    if (!active_ || closed_.load())
    {
        // ---- CaptureItem 已关闭 → 采集失效 ----
        if (closed_.load() && active_)
        {
            active_ = false;
        }
        return false;
    }

    // ---- 没有 FrameArrived 事件 → 无新帧 ----
    if (!new_frame_arrived_.exchange(false))
    {
        return false;
    }

    // ---- 从 FramePool 获取下一帧 ----
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame* frame = nullptr;
    HRESULT hr = frame_pool_->TryGetNextFrame(&frame);
    if (FAILED(hr) || !frame)
    {
        return false;
    }

    // ---- 获取帧的 Surface（IDirect3DSurface） ----
    ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface* surface = nullptr;
    hr = frame->get_Surface(&surface);
    frame->Release();

    if (FAILED(hr) || !surface)
    {
        return false;
    }

    // ---- 将 IDirect3DSurface 转为 ID3D11Texture2D ----
    // IDirect3DSurface 实际上就是 ID3D11Texture2D 的包装，通过 QI 获取
    ID3D11Texture2D* texture = nullptr;
    hr = surface->QueryInterface(__uuidof(ID3D11Texture2D),
                                  reinterpret_cast<void**>(&texture));
    surface->Release();

    if (FAILED(hr) || !texture)
    {
        return false;
    }

    // ---- 更新尺寸 ----
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    width_ = desc.Width;
    height_ = desc.Height;

    // ---- 加锁替换当前帧纹理 ----
    {
        std::lock_guard<std::mutex> lock(texture_mutex_);
        if (current_texture_)
        {
            current_texture_->Release();
        }
        current_texture_ = texture;
    }

    return true;
}

// ========== 获取当前帧纹理 ==========

ID3D11Texture2D* WgcCapture::GetTexture() const
{
    std::lock_guard<std::mutex> lock(texture_mutex_);
    return current_texture_;
}

// ========== 查询方法 ==========

uint32_t WgcCapture::Width() const
{
    return width_;
}

uint32_t WgcCapture::Height() const
{
    return height_;
}

bool WgcCapture::IsActive() const
{
    return active_;
}

// ========== 动态切换光标显隐 ==========

bool WgcCapture::ShowCursor(bool visible)
{
    if (!session2_)
    {
        return false;
    }

    // ---- put_IsCursorCaptureEnabled 在 IGraphicsCaptureSession2 上 ----
    HRESULT hr = session2_->put_IsCursorCaptureEnabled(visible ? TRUE : FALSE);
    return SUCCEEDED(hr);
}
