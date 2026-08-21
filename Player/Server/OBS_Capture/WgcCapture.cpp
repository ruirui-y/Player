#include "WgcCapture.h"

#include "Common/LogManager.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <roapi.h>
#include <wrl.h>

#include <windows.graphics.directx.direct3d11.interop.h>   // 声明 CreateDirect3D11DeviceFromDXGIDevice

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windows.graphics.directx.direct3d11.lib")   // 链接导入库，禁止 LoadLibrary 按 API 集名加载（会 126）

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
        LogManager::Log("ERR", "[WgcCapture] QI IDXGIDevice 失败: hr=0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 直接调用 CreateDirect3D11DeviceFromDXGIDevice ----
    // 注意：该函数在 windows.graphics.directx.direct3d11.dll 中，但它是 Win32 API 集，
    // 不能用 LoadLibrary 按文件名运行时加载（会 126 ERROR_MOD_NOT_FOUND）。
    // 正确做法是链接 SDK 导入库 windows.graphics.directx.direct3d11.lib（见文件顶部 pragma）。
    IInspectable* inspectable = nullptr;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device_, &inspectable);

    if (FAILED(hr) || !inspectable)
    {
        LogManager::Log("ERR", "[WgcCapture] CreateDirect3D11DeviceFromDXGIDevice 失败: hr=0x%08X", (unsigned)hr);
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
        LogManager::Log("ERR", "[WgcCapture] CreateItemForMonitor: RoGetActivationFactory 失败 hr=0x%08X (未 RoInitialize? 0x800401F0)",
                        (unsigned)hr);
        return false;
    }

    // ---- 通过 Interop 从 HMONITOR 创建 CaptureItem ----
    hr = interop->CreateForMonitor(monitor,
                                    __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
                                    reinterpret_cast<void**>(&item_));
    interop->Release();

    if (FAILED(hr) || !item_)
    {
        LogManager::Log("ERR", "[WgcCapture] CreateItemForMonitor: CreateForMonitor 失败 hr=0x%08X", (unsigned)hr);
    }
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
        LogManager::Log("ERR", "[WgcCapture] CreateItemForWindow: RoGetActivationFactory 失败 hr=0x%08X (未 RoInitialize? 0x800401F0)",
                        (unsigned)hr);
        return false;
    }

    // ---- 通过 Interop 从 HWND 创建 CaptureItem ----
    hr = interop->CreateForWindow(window,
                                   __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
                                   reinterpret_cast<void**>(&item_));
    interop->Release();

    if (FAILED(hr) || !item_)
    {
        LogManager::Log("ERR", "[WgcCapture] CreateItemForWindow: CreateForWindow 失败 hr=0x%08X", (unsigned)hr);
    }
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
        LogManager::Log("ERR", "[WgcCapture] StartCapture: RoGetActivationFactory(FramePool) 失败 hr=0x%08X", (unsigned)hr);
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
        LogManager::Log("ERR", "[WgcCapture] StartCapture: FramePool::Create 失败 hr=0x%08X", (unsigned)hr);
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
        LogManager::Log("ERR", "[WgcCapture] StartCapture: add_FrameArrived 失败 hr=0x%08X", (unsigned)hr);
        return false;
    }

    // ---- 注册 CaptureItem Closed 事件 ----
    auto closed_handler = Microsoft::WRL::Make<CaptureClosedHandler>(&closed_);
    hr = item_->add_Closed(closed_handler.Get(), &closed_token_);
    if (FAILED(hr))
    {
        // Closed 事件注册失败不影响核心功能
        LogManager::Log("DBG", "[WgcCapture] StartCapture: add_Closed 失败(忽略) hr=0x%08X", (unsigned)hr);
    }

    // ---- 创建 CaptureSession ----
    hr = frame_pool_->CreateCaptureSession(item_, &session_);
    if (FAILED(hr) || !session_)
    {
        LogManager::Log("ERR", "[WgcCapture] StartCapture: CreateCaptureSession 失败 hr=0x%08X", (unsigned)hr);
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

// ========== 初始化（CaptureBackend 接口，合并 Monitor/Window 两条路线） ==========

bool WgcCapture::Init(const BackendContext& ctx)
{
    Shutdown();

    if (!ctx.device)
    {
        LogManager::Log("ERR", "[WgcCapture] Init 失败: ctx.device 为空");
        return false;
    }

    device_ = ctx.device;
    cursor_ = ctx.capture_cursor;
    force_sdr_ = ctx.force_sdr;
    client_area_ = ctx.client_area;

    // ---- 第一步：创建 WinRT Direct3D 设备 ----
    LogManager::Log("DBG", "[WgcCapture] Init 第一步: CreateDirect3DDeviceFromD3D11");
    if (!CreateDirect3DDeviceFromD3D11(device_))
    {
        LogManager::Log("ERR", "[WgcCapture] Init 失败: 第一步 CreateDirect3DDeviceFromD3D11 返回 false");
        Shutdown();
        return false;
    }

    // ---- 第二步：创建 GraphicsCaptureItem（窗口路线优先，否则显示器路线） ----
    if (ctx.window)
    {
        LogManager::Log("DBG", "[WgcCapture] Init 第二步: CreateItemForWindow hwnd=%p", (void*)ctx.window);
    }
    else
    {
        LogManager::Log("DBG", "[WgcCapture] Init 第二步: CreateItemForMonitor hmonitor=%p", (void*)ctx.monitor);
    }
    bool item_ok = ctx.window ? CreateItemForWindow(ctx.window)
                              : CreateItemForMonitor(ctx.monitor);
    if (!item_ok)
    {
        LogManager::Log("ERR", "[WgcCapture] Init 失败: 第二步 创建 GraphicsCaptureItem 返回 false (window=%d)",
                        ctx.window ? 1 : 0);
        Shutdown();
        return false;
    }

    // ---- 第三步：创建 FramePool + Session 并启动 ----
    LogManager::Log("DBG", "[WgcCapture] Init 第三步: StartCapture");
    if (!StartCapture())
    {
        LogManager::Log("ERR", "[WgcCapture] Init 失败: 第三步 StartCapture 返回 false");
        Shutdown();
        return false;
    }

    LogManager::Log("INFO", "[WgcCapture] Init 成功: %ux%u cursor=%d force_sdr=%d client_area=%d",
                    width_, height_, cursor_ ? 1 : 0, force_sdr_ ? 1 : 0, client_area_ ? 1 : 0);
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

// ========== 采集一帧（CaptureBackend 接口） ==========

bool WgcCapture::AcquireFrame()
{
    // ---- WGC 为推模式：泵一下内部 Tick，只要 backend 仍活跃即返回 true ----
    // ---- 区分于 DXGI：Tick 返回 false 仅表示“本帧无新画面”，不代表 backend 死亡 ----
    Tick();
    return IsActive();
}

// ========== 获取当前帧（CaptureBackend 接口） ==========

bool WgcCapture::GetFrame(CaptureFrame& out)
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
    out.rotation = 0;
    return true;
}

// ========== 设置光标隐藏（CaptureBackend 接口） ==========

void WgcCapture::SetCursorHidden(bool hidden)
{
    ShowCursor(!hidden);
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
