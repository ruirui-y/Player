#include "VideoRenderer.h"
#include <QDebug>

extern "C"
{
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

VideoRenderer::VideoRenderer(QObject* parent) : QObject(parent) {}
VideoRenderer::~VideoRenderer() { ReleaseD3D11(); }
void VideoRenderer::SetHwnd(HWND hwnd) { hwnd_ = hwnd; }
ID3D11Device* VideoRenderer::GetD3D11Device() const { return d3d11_device_; }
ID3D11DeviceContext* VideoRenderer::GetD3D11DeviceContext() const { return d3d11_ctx_; }

// ---- 创建 D3D11 设备 + 交换链（用于纯软解或自主渲染时的独立初始化） ----
ID3D11Device* VideoRenderer::InitD3D11(int width, int height)
{
    ReleaseD3D11();

    if (!hwnd_) {
        qDebug() << "[VideoRenderer] InitD3D11 失败: 窗口句柄 hwnd_ 为空";
        return nullptr;
    }

    qDebug() << "[VideoRenderer] 开始独立初始化 D3D11 设备与交换链，分辨率:" << width << "x" << height;

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    // 创建硬渲染所需的 D3D11 设备与上下文
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &device, nullptr, &ctx);

    if (FAILED(hr)) {
        qDebug() << "[VideoRenderer] D3D11CreateDevice 失败，HRESULT:" << hr;
        return nullptr;
    }

    // ---- 从 D3D 设备向上查询 DXGI 工厂，用于创建交换链 ----
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;

    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev);
    dxgi_dev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);

    // 配置交换链结构体
    DXGI_SWAP_CHAIN_DESC scd = { 0 };
    scd.BufferCount = 2;                                    // 双缓冲
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;     // 常用标准的 RGBA 格式
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;      // 作为渲染目标输出
    scd.OutputWindow = hwnd_;                                // 绑定的窗口句柄
    scd.SampleDesc.Count = 1;                               // 关闭抗锯齿以提升播放性能
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;         // Win10及以上推荐的现代高效翻转模式

    IDXGISwapChain* sc = nullptr;
    hr = factory->CreateSwapChain(device, &scd, &sc);

    // 释放临时 DXGI 接口引用
    dxgi_dev->Release();
    adapter->Release();
    factory->Release();

    if (FAILED(hr))
    {
        qDebug() << "[VideoRenderer] 独立初始化创建交换链失败，HRESULT:" << hr;
        device->Release();
        ctx->Release();
        return nullptr;
    }

    // 赋值给类的私有成员管理
    d3d11_device_ = device;
    d3d11_ctx_ = ctx;
    swapchain_ = sc;

    frame_width_ = width;
    frame_height_ = height;

    qDebug() << "[VideoRenderer] 独立 D3D11 设备与交换链初始化成功，软解渲染管线已打通";
    return device;
}

void VideoRenderer::ReleaseD3D11()
{
    if (swapchain_) { static_cast<IDXGISwapChain*>(swapchain_)->Release(); swapchain_ = nullptr; }
    if (d3d11_ctx_) { d3d11_ctx_->Release(); d3d11_ctx_ = nullptr; }
    if (d3d11_device_) { d3d11_device_->Release(); d3d11_device_ = nullptr; }
    qDebug() << "[VideoRenderer] D3D11 渲染资源已释放";
}

bool VideoRenderer::CreateSwapChain(ID3D11Device* device, int width, int height)
{
    ReleaseD3D11();
    if (!device || !hwnd_) return false;

    qDebug() << "[VideoRenderer] 利用解码器传入的 ID3D11Device 创建交换链, 尺寸:" << width << "x" << height;

    d3d11_device_ = device;
    device->AddRef();
    device->GetImmediateContext(&d3d11_ctx_); // 关键：获取 Context 用于后续的 CopySubresourceRegion

    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;

    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev);
    dxgi_dev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);

    DXGI_SWAP_CHAIN_DESC scd = { 0 };
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd_;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* sc = nullptr;
    HRESULT hr = factory->CreateSwapChain(device, &scd, &sc);

    dxgi_dev->Release();
    adapter->Release();
    factory->Release();

    if (FAILED(hr)) {
        qDebug() << "[VideoRenderer] 创建交换链失败，HRESULT:" << hr;
        return false;
    }

    swapchain_ = sc;
    frame_width_ = width;
    frame_height_ = height;
    qDebug() << "[VideoRenderer] 交换链创建成功，硬解管线打通";
    return true;
}

// ---- 统一渲染入口：混合管线适配 ----
void VideoRenderer::Render(AVFrame* frame)
{
    if (!frame) return;

    int fmt = frame->format;

    // 【核心揭秘】硬解出来的格式是 D3D11 专属的 NV12 纹理，不能直接 Copy 到 RGB 的交换链！
    // 真正的全 GPU 管线需要写 HLSL Shader。现在我们暂时走“GPU显存下载 -> CPU转RGB -> 显示”的混合管线

    // 把现代的 AV_PIX_FMT_D3D11 也加到拦截列表里
    if (fmt == AV_PIX_FMT_D3D11 || fmt == AV_PIX_FMT_D3D11VA_VLD || fmt == AV_PIX_FMT_DXVA2_VLD)
    {
        AVFrame* sw_frame = av_frame_alloc();
        if (sw_frame)
        {
            // 将 GPU 显存中的 NV12 画面下载到 CPU 系统内存
            int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
            if (ret == 0)
            {
                RenderSoftware(sw_frame); // 交给 sws_scale 进行 YUV->RGB 转换并发射 QImage
            }
            else
            {
                qDebug() << "[VideoRenderer] 硬件帧下载到系统内存失败:" << ret;
            }
            av_frame_free(&sw_frame);
        }
        return;
    }

    // 纯软解格式：直接渲染
    RenderSoftware(frame);
}

void VideoRenderer::RenderHardware(AVFrame* frame)
{
    if (!d3d11_ctx_ || !swapchain_) return;

    // 核心修正：摒弃自定义 Struct，直接取值
    ID3D11Texture2D* dec_tex = (ID3D11Texture2D*)frame->data[0];
    intptr_t subresource_idx = (intptr_t)frame->data[1];

    if (!dec_tex) {
        qDebug() << "[VideoRenderer] 严重错误: frame->data[0] 为空";
        return;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    if (FAILED(hr)) return;

    // 核心修正：使用 CopySubresourceRegion 精确拷贝纹理数组中的切片
    d3d11_ctx_->CopySubresourceRegion(
        backbuffer, 0, 0, 0, 0,
        dec_tex, (UINT)subresource_idx, nullptr
    );

    swapchain_->Present(0, 0);
    backbuffer->Release();
}

void VideoRenderer::RenderSoftware(AVFrame* frame)
{
    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame->format);
    int w = frame->width;
    int h = frame->height;

    if (!sws_ctx_ || w != frame_width_ || h != frame_height_) {
        qDebug() << "[VideoRenderer] (重新)初始化 SwsContext: 尺寸" << w << "x" << h << " 格式:" << src_fmt;
        if (sws_ctx_) {
            sws_freeContext(static_cast<SwsContext*>(sws_ctx_));
            sws_ctx_ = nullptr;
        }
        sws_ctx_ = sws_getContext(w, h, src_fmt, w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        frame_width_ = w;
        frame_height_ = h;

        // ▼ 加这行防御：如果创建失败，直接抛弃这一帧，避免一直崩溃死循环 ▼
        if (!sws_ctx_) {
            qDebug() << "[VideoRenderer] 致命错误: sws_getContext 创建失败！";
            return;
        }
    }
    if (!sws_ctx_) return;

    QImage image(w, h, QImage::Format_RGB888);
    uint8_t* dst[1] = { image.bits() };
    int      dst_stride = static_cast<int>(image.bytesPerLine());

    sws_scale(static_cast<SwsContext*>(sws_ctx_), frame->data, frame->linesize, 0, h, dst, &dst_stride);
    emit SigFrameReady(image);
}