#include "VideoRenderer.h"
#include <QDebug>

#include <d3dcompiler.h>

extern "C"
{
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

const char* shader_code = R"(
Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState samLinear : register(s0);

struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};

// 顶点着色器：利用顶点 ID 凭空生成一个覆盖全屏的大三角形
VS_OUTPUT VS(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.Tex = float2((id << 1) & 2, id & 2);
    output.Pos = float4(output.Tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

// 像素着色器：采样 Y 和 UV，转换为 RGB
float4 PS(VS_OUTPUT input) : SV_Target {
    float y = texY.Sample(samLinear, input.Tex).r;
    float2 uv = texUV.Sample(samLinear, input.Tex).rg - 0.5;

    // BT.709 色彩转换公式
    float r = y + 1.5748 * uv.y;
    float g = y - 0.1873 * uv.x - 0.4681 * uv.y;
    float b = y + 1.8556 * uv.x;

    return float4(r, g, b, 1.0);
}
)";

VideoRenderer::VideoRenderer(QObject* parent) : QObject(parent) {}
VideoRenderer::~VideoRenderer()
{
    // === 安全释放 Shader 相关的三个 D3D11 资源 ===
    if (vertex_shader_) {
        vertex_shader_->Release();
        vertex_shader_ = nullptr;
    }
    if (pixel_shader_) {
        pixel_shader_->Release();
        pixel_shader_ = nullptr;
    }
    if (sampler_linear_) {
        sampler_linear_->Release();
        sampler_linear_ = nullptr;
    }
    if (srv_uv_) { srv_uv_->Release(); srv_uv_ = nullptr; }
    if (srv_y_) { srv_y_->Release(); srv_y_ = nullptr; }
    if (staging_uv_texture_) { staging_uv_texture_->Release(); staging_uv_texture_ = nullptr; }
    if (staging_y_texture_) { staging_y_texture_->Release(); staging_y_texture_ = nullptr; }
    // =================================================

    ReleaseD3D11();
}
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
    if (srv_uv_) { srv_uv_->Release(); srv_uv_ = nullptr; }
    if (srv_y_) { srv_y_->Release(); srv_y_ = nullptr; }
    if (staging_uv_texture_) { staging_uv_texture_->Release(); staging_uv_texture_ = nullptr; }
    if (staging_y_texture_) { staging_y_texture_->Release(); staging_y_texture_ = nullptr; }
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
    device->GetImmediateContext(&d3d11_ctx_);

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

    // === 交换链创建成功后，立刻初始化 Shader 渲染管线 ===
    if (!InitShaders()) {
        qDebug() << "[VideoRenderer] 严重错误：Shader 编译或初始化失败！";
        return false;
    }
    // =========================================================

    // ---- 创建两个独立的中转纹理：Y 平面 + UV 平面 ----
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DYNAMIC;
    tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // Y 平面纹理（R8_UNORM，尺寸跟视频一样）
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(d3d11_device_->CreateTexture2D(&tex_desc, nullptr, &staging_y_texture_)))
    {
        qDebug() << "[VideoRenderer] 创建 Y 平面纹理失败";
        return false;
    }

    // UV 平面纹理（R8G8_UNORM，NV12 的 UV 是宽高各一半）
    tex_desc.Width = width / 2;
    tex_desc.Height = height / 2;
    tex_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(d3d11_device_->CreateTexture2D(&tex_desc, nullptr, &staging_uv_texture_)))
    {
        qDebug() << "[VideoRenderer] 创建 UV 平面纹理失败";
        return false;
    }

    // ---- 创建 SRV ----
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    srv_desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(d3d11_device_->CreateShaderResourceView(staging_y_texture_, &srv_desc, &srv_y_)))
    {
        qDebug() << "[VideoRenderer] 创建 Y SRV 失败";
        return false;
    }

    srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(d3d11_device_->CreateShaderResourceView(staging_uv_texture_, &srv_desc, &srv_uv_)))
    {
        qDebug() << "[VideoRenderer] 创建 UV SRV 失败";
        return false;
    }

    qDebug() << "[VideoRenderer] 交换链、着色器、双平面纹理全部就绪，纯 GPU 管线已打通！";
    return true;
}

void VideoRenderer::Render(AVFrame* frame)
{
    if (!frame) return;

    int fmt = frame->format;

    // 纯 GPU 管线：D3D11 格式 + 着色器 + SRV 就绪 → 走着色器渲染
    if (fmt == AV_PIX_FMT_D3D11 && swapchain_ && pixel_shader_ && srv_y_)
    {
        RenderHardware(frame);
        return;
    }

    // D3D11 格式但 GPU 管线不可用（SRV 创建失败）→ 下载到 CPU 后走软解
    if (fmt == AV_PIX_FMT_D3D11)
    {
        AVFrame* sw_frame = av_frame_alloc();
        if (sw_frame)
        {
            int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
            if (ret == 0)
            {
                RenderSoftware(sw_frame);
            }
            else
            {
                qDebug() << "[VideoRenderer] 硬件帧下载到系统内存失败:" << ret;
            }
            av_frame_free(&sw_frame);
        }
        return;
    }

    // DXVA2 格式 → 下载到 CPU 后走软解
    if (fmt == AV_PIX_FMT_D3D11VA_VLD || fmt == AV_PIX_FMT_DXVA2_VLD)
    {
        AVFrame* sw_frame = av_frame_alloc();
        if (sw_frame)
        {
            int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
            if (ret == 0)
            {
                RenderSoftware(sw_frame);
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
    if (!d3d11_ctx_ || !swapchain_ || !srv_y_ || !srv_uv_) return;

    // 获取交换链后备缓冲
    ID3D11Texture2D* backbuffer = nullptr;
    swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);

    ID3D11RenderTargetView* rtv = nullptr;
    d3d11_device_->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    backbuffer->Release();

    d3d11_ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)frame_width_, (float)frame_height_, 0.0f, 1.0f };
    d3d11_ctx_->RSSetViewports(1, &vp);

    // ==========================================
    // 将帧从 GPU 显存下载到 CPU 内存
    // ==========================================
    AVFrame* sw_frame = av_frame_alloc();
    if (!sw_frame) { rtv->Release(); return; }

    int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
    if (ret != 0 || !sw_frame->data[0] || !sw_frame->data[1])
    {
        qDebug() << "RenderHardware: av_hwframe_transfer_data failed" << ret;
        av_frame_free(&sw_frame);
        rtv->Release();
        return;
    }

    // ==========================================
    // 将 Y 平面从 CPU 内存上传到 GPU 纹理
    // ==========================================
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(d3d11_ctx_->Map(staging_y_texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        // Y 平面是单通道，linesize 可能不等于 width（有对齐）
        int y_height = sw_frame->height;
        for (int row = 0; row < y_height; row++)
        {
            memcpy((uint8_t*)mapped.pData + row * mapped.RowPitch,
                sw_frame->data[0] + row * sw_frame->linesize[0],
                sw_frame->width);
        }
        d3d11_ctx_->Unmap(staging_y_texture_, 0);
    }

    // ==========================================
    // 将 UV 平面从 CPU 内存上传到 GPU 纹理
    // ==========================================
    if (SUCCEEDED(d3d11_ctx_->Map(staging_uv_texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        // UV 平面是双通道（R8G8），高度是视频高度的一半
        int uv_height = sw_frame->height / 2;
        int uv_width_bytes = sw_frame->width;  // UV 的 linesize 跟 width 一致（2 字节每像素）
        for (int row = 0; row < uv_height; row++)
        {
            memcpy((uint8_t*)mapped.pData + row * mapped.RowPitch,
                sw_frame->data[1] + row * sw_frame->linesize[1],
                uv_width_bytes);
        }
        d3d11_ctx_->Unmap(staging_uv_texture_, 0);
    }

    av_frame_free(&sw_frame);

    // ==========================================
    // 渲染管线装配与执行
    // ==========================================
    d3d11_ctx_->IASetInputLayout(nullptr);
    d3d11_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    d3d11_ctx_->VSSetShader(vertex_shader_, nullptr, 0);
    d3d11_ctx_->PSSetShader(pixel_shader_, nullptr, 0);

    ID3D11ShaderResourceView* srvs[] = { srv_y_, srv_uv_ };
    d3d11_ctx_->PSSetShaderResources(0, 2, srvs);
    d3d11_ctx_->PSSetSamplers(0, 1, &sampler_linear_);

    d3d11_ctx_->Draw(3, 0);
    swapchain_->Present(1, 0);

    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
    d3d11_ctx_->PSSetShaderResources(0, 2, null_srvs);
    rtv->Release();
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

bool VideoRenderer::InitShaders()
{
    if (!d3d11_device_) return false;

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    // 编译顶点着色器
    D3DCompile(shader_code, strlen(shader_code), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs_blob, &error_blob);
    if (error_blob) {
        qDebug() << "VS Compile Error:" << (char*)error_blob->GetBufferPointer();
        error_blob->Release();
        return false;
    }
    d3d11_device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_);
    vs_blob->Release();

    // 编译像素着色器
    D3DCompile(shader_code, strlen(shader_code), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps_blob, &error_blob);
    d3d11_device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader_);
    ps_blob->Release();

    // 创建线性采样器
    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d3d11_device_->CreateSamplerState(&samp_desc, &sampler_linear_);

    return true;
}