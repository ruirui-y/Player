#include "VideoRenderer.h"
#include <QDebug>

#include <d3dcompiler.h>

extern "C"
{
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>    // AVD3D11VAFrame 定义在此
#include <libswscale/swscale.h>
}

// ---- 内嵌 HLSL 着色器代码 ----
// 顶点着色器用 SV_VertexID 凭空生成全屏三角形，不需要顶点缓冲区
// 像素着色器接收两张纹理（Y + UV），做 NV12→RGB 的 BT.709 色彩转换
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

VideoRenderer::VideoRenderer(QObject* parent)
    : QObject(parent)
{
}

VideoRenderer::~VideoRenderer()
{
    if (vertex_shader_) { vertex_shader_->Release();        vertex_shader_ = nullptr; }
    if (pixel_shader_) { pixel_shader_->Release();         pixel_shader_ = nullptr; }
    if (sampler_linear_) { sampler_linear_->Release();       sampler_linear_ = nullptr; }
    if (test_nv12_srv_) { test_nv12_srv_->Release();        test_nv12_srv_ = nullptr; }
    if (test_nv12_texture_) { test_nv12_texture_->Release();  test_nv12_texture_ = nullptr; }
    if (nv12_srv_uv_) { nv12_srv_uv_->Release();            nv12_srv_uv_ = nullptr; }
    if (nv12_srv_y_) { nv12_srv_y_->Release();             nv12_srv_y_ = nullptr; }
    if (nv12_texture_) { nv12_texture_->Release();          nv12_texture_ = nullptr; }
    if (srv_uv_) { srv_uv_->Release();               srv_uv_ = nullptr; }
    if (srv_y_) { srv_y_->Release();                srv_y_ = nullptr; }
    if (staging_uv_texture_) { staging_uv_texture_->Release();   staging_uv_texture_ = nullptr; }
    if (staging_y_texture_) { staging_y_texture_->Release();    staging_y_texture_ = nullptr; }
    if (d3d11_device3_) { d3d11_device3_->Release(); d3d11_device3_ = nullptr; }
    ReleaseD3D11();
}

void VideoRenderer::SetHwnd(HWND hwnd) { hwnd_ = hwnd; }
ID3D11Device* VideoRenderer::GetD3D11Device() const { return d3d11_device_; }
ID3D11DeviceContext* VideoRenderer::GetD3D11DeviceContext() const { return d3d11_ctx_; }
void VideoRenderer::SetUploadStrategy(UploadStrategy s) { active_strategy_ = s; }
VideoRenderer::UploadStrategy VideoRenderer::GetUploadStrategy() const { return active_strategy_; }

// 独立创建 D3D11 设备 + 交换链（用于纯软解模式下创建独立的渲染环境）
ID3D11Device* VideoRenderer::InitD3D11(int width, int height)
{
    ReleaseD3D11();

    if (!hwnd_)
    {
        qDebug() << "[VideoRenderer] InitD3D11 失败: 窗口句柄 hwnd_ 为空";
        return nullptr;
    }

    qDebug() << "[VideoRenderer] 开始独立初始化 D3D11 设备与交换链，分辨率:"
        << width << "x" << height;

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &device, nullptr, &ctx);

    if (FAILED(hr))
    {
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
    hr = factory->CreateSwapChain(device, &scd, &sc);

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
    // 按依赖顺序反向释放：SRV → 纹理 → 交换链 → 上下文 → 设备
    if (nv12_srv_uv_) { nv12_srv_uv_->Release();            nv12_srv_uv_ = nullptr; }
    if (nv12_srv_y_) { nv12_srv_y_->Release();             nv12_srv_y_ = nullptr; }
    if (nv12_texture_) { nv12_texture_->Release();          nv12_texture_ = nullptr; }
    if (test_nv12_srv_) { test_nv12_srv_->Release();        test_nv12_srv_ = nullptr; }
    if (test_nv12_texture_) { test_nv12_texture_->Release();  test_nv12_texture_ = nullptr; }
    if (srv_uv_) { srv_uv_->Release();               srv_uv_ = nullptr; }
    if (srv_y_) { srv_y_->Release();                srv_y_ = nullptr; }
    if (staging_uv_texture_) { staging_uv_texture_->Release();   staging_uv_texture_ = nullptr; }
    if (staging_y_texture_) { staging_y_texture_->Release();    staging_y_texture_ = nullptr; }
    if (swapchain_) { static_cast<IDXGISwapChain*>(swapchain_)->Release(); swapchain_ = nullptr; }
    if (d3d11_ctx_) { d3d11_ctx_->Release();            d3d11_ctx_ = nullptr; }
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (d3d11_device_) { d3d11_device_->Release();         d3d11_device_ = nullptr; }
    if (d3d11_device3_) { d3d11_device3_->Release(); d3d11_device3_ = nullptr; }
    qDebug() << "[VideoRenderer] D3D11 渲染资源已释放";
}

// ================================================================
// 创建交换链
// ================================================================
bool VideoRenderer::CreateSwapChain(ID3D11Device* device, int width, int height)
{
    // ---- 第一步：清理旧资源 ----
    ReleaseD3D11();
    if (!device || !hwnd_)
    {
        qDebug() << "[VideoRenderer] CreateSwapChain 失败: device 或 hwnd_ 为空";
        return false;
    }

    qDebug() << "[VideoRenderer] 开始创建渲染管线，分辨率:" << width << "x" << height;

    // 保存设备指针，增加引用计数防止外部意外释放
    d3d11_device_ = device;
    device->AddRef();
    // 获取设备上下文，后续 Map/Unmap/Draw/Present 都靠它
    device->GetImmediateContext(&d3d11_ctx_);

    // 查询 ID3D11Device3（不是 Device1）
    HRESULT hr = d3d11_device_->QueryInterface(
        __uuidof(ID3D11Device3), (void**)&d3d11_device3_);

    // ---- 第二步：创建交换链（显卡→显示器的桥梁） ----
    if (!CreateD3D11SwapChain(device, width, height))
    {
        qDebug() << "[VideoRenderer] 创建交换链失败";
        return false;
    }

    // ---- 第三步：编译 HLSL 着色器（GPU 执行的 NV12→RGB 转换程序） ----
    if (!InitShaders())
    {
        qDebug() << "[VideoRenderer] 着色器初始化失败";
        return false;
    }

    // ---- 第四步：创建两块中转纹理（CPU 写→GPU 读的传声筒） ----
    if (!CreateStagingTextures(width, height))
    {
        qDebug() << "[VideoRenderer] 创建中转纹理失败";
        return false;
    }

    // ---- 第五步：创建着色器资源视图 SRV（告诉着色器怎么读纹理） ----
    if (!CreateShaderResourceViews())
    {
        qDebug() << "[VideoRenderer] 创建 SRV 失败";
        return false;
    }

    qDebug() << "[VideoRenderer] 渲染管线全部就绪，纯 GPU 管线已打通！";
    return true;
}

// ================================================================
// 创建交换链
// 交换链 = 显卡和显示器之间的"双缓冲画板"
// 显卡往后台画板画，画完后 Present 交换，显示器看前台画板
// ================================================================
bool VideoRenderer::CreateD3D11SwapChain(ID3D11Device* device,
    int width, int height)
{
    // ---- 从 D3D11 设备向上查询 DXGI 工厂 ----
    // 设备→适配器（显卡）→工厂（管理交换链的对象）
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;

    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev);
    dxgi_dev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);

    // ---- 配置交换链的各项参数 ----
    DXGI_SWAP_CHAIN_DESC scd = { 0 };

    scd.BufferCount = 2;                                            // 两个缓冲（双缓冲）
    scd.BufferDesc.Width = width;                                   // 画面宽度
    scd.BufferDesc.Height = height;                                 // 画面高度
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;             // 每个像素 4 字节 RGBA
    scd.BufferDesc.RefreshRate.Numerator = 60;                      // 刷新率 60Hz
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;              // 作为渲染目标输出
    scd.OutputWindow = hwnd_;                                       // 绑定到哪个窗口
    scd.SampleDesc.Count = 1;                                       // 不启用抗锯齿
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;                                            // 窗口模式（不是全屏独占）
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;                 // Win10 推荐的现代翻转模式

    // ---- 让 DXGI 工厂创建交换链 ----
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = factory->CreateSwapChain(device, &scd, &sc);

    // ---- 释放临时接口 ----
    dxgi_dev->Release();
    adapter->Release();
    factory->Release();

    if (FAILED(hr))
    {
        qDebug() << "[VideoRenderer] CreateSwapChain 失败，HRESULT:" << hr;
        return false;
    }

    swapchain_ = sc;
    frame_width_ = width;
    frame_height_ = height;

    // ---- 创建渲染目标视图 RTV ----
    ID3D11Texture2D* backbuffer = nullptr;
    swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    d3d11_device_->CreateRenderTargetView(backbuffer, nullptr, &rtv_);
    backbuffer->Release();

    qDebug() << "[VideoRenderer] 交换链创建成功";
    return true;
}

// ================================================================
// 创建两个中转纹理
// 解码器解出来的 NV12 帧在显存里，但我们不能直接读取它
// 所以需要把 Y 和 UV 分别复制到两张独立的纹理上
//
// 为什么用 DYNAMIC + CPU_ACCESS_WRITE：
//   这些纹理每帧都会从 CPU 写入新数据（memcpy）
//   DYNAMIC 是专门为"CPU 频繁写入"优化的纹理类型
// ================================================================
bool VideoRenderer::CreateStagingTextures(int width, int height)
{
    D3D11_TEXTURE2D_DESC tex_desc = {};

    tex_desc.MipLevels = 1;                                     // 不使用 Mipmap
    tex_desc.ArraySize = 1;                                     // 单张纹理，不是纹理数组
    tex_desc.SampleDesc.Count = 1;                              // 不抗锯齿
    tex_desc.Usage = D3D11_USAGE_DYNAMIC;                       // CPU 写入 + GPU 读取
    tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;           // 允许 CPU 写入
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;            // 能被着色器读取

    // ---- Y 平面纹理 ----
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(d3d11_device_->CreateTexture2D(&tex_desc, nullptr, &staging_y_texture_)))
    {
        qDebug() << "[VideoRenderer] 创建 Y 平面纹理失败";
        return false;
    }

    // ---- UV 平面纹理 ----
    tex_desc.Width = width / 2;
    tex_desc.Height = height / 2;
    tex_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(d3d11_device_->CreateTexture2D(&tex_desc, nullptr, &staging_uv_texture_)))
    {
        qDebug() << "[VideoRenderer] 创建 UV 平面纹理失败";
        return false;
    }

    qDebug() << "[VideoRenderer] 中转纹理创建成功: Y=" << width << "x" << height
        << " UV=" << width / 2 << "x" << height / 2;
    return true;
}

// ================================================================
// 创建着色器资源视图 SRV
//
// 纹理建好了，但着色器不能直接用纹理
// 需要一个"视图"作为中间层，告诉着色器：
//   - 这个纹理是什么格式（R8 还是 R8G8）
//   - 是 2D 纹理还是纹理数组
//   - 读哪一层 Mipmap
// 这个中间层就是 SRV（Shader Resource View）
// ================================================================
bool VideoRenderer::CreateShaderResourceViews()
{
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};

    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    // ---- Y 平面的 SRV ----
    srv_desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(d3d11_device_->CreateShaderResourceView(
        staging_y_texture_, &srv_desc, &srv_y_)))
    {
        qDebug() << "[VideoRenderer] 创建 Y SRV 失败";
        return false;
    }

    // ---- UV 平面的 SRV ----
    srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(d3d11_device_->CreateShaderResourceView(
        staging_uv_texture_, &srv_desc, &srv_uv_)))
    {
        qDebug() << "[VideoRenderer] 创建 UV SRV 失败";
        return false;
    }

    qDebug() << "[VideoRenderer] 着色器资源视图创建成功";
    return true;
}

// ---- 统一渲染入口：根据帧格式分派不同渲染路径 ----
void VideoRenderer::Render(AVFrame* frame)
{
    if (!frame) return;

    int fmt = frame->format;

    // 路径一：硬解帧 → 走 GPU 硬件渲染（通过 UploadStrategy 切换上传方案）
    if (fmt == AV_PIX_FMT_D3D11 && swapchain_ && pixel_shader_)
    {
        RenderHardware(frame);
        return;
    }

    // 路径二：D3D11 格式但 GPU 管线不可用 → 下载到 CPU 后 sws_scale 软解
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

    // 路径三：老格式（DXVA2）→ 下载到 CPU 后软解
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

    // 路径四：纯软解帧，直接渲染
    RenderSoftware(frame);
}

// ================================================================
// 策略 A：GPU→CPU 下载 → CPU 拆 NV12 → memcpy 上传两张 R8/R8G8 纹理
// ================================================================
VideoRenderer::UploadResult VideoRenderer::UploadViaCPUTransfer(AVFrame* frame)
{
    UploadResult result;
    result.strategy_name = "A: GPU→CPU下载→CPU拆NV12→memcpy上传";

    auto t0 = std::chrono::steady_clock::now();

    // ---- 第①步：GPU→CPU 下载 ----
    AVFrame* sw_frame = av_frame_alloc();
    if (!sw_frame)
    {
        result.detail = "av_frame_alloc 失败";
        return result;
    }

    int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
    if (ret != 0 || !sw_frame->data[0] || !sw_frame->data[1])
    {
        result.detail = QString("av_hwframe_transfer_data 失败, ret=%1").arg(ret);
        av_frame_free(&sw_frame);
        return result;
    }

    // ---- 第②步：Y 平面逐行上传 ----
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(d3d11_ctx_->Map(
        staging_y_texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        int y_height = sw_frame->height;
        for (int row = 0; row < y_height; row++)
        {
            memcpy((uint8_t*)mapped.pData + row * mapped.RowPitch,
                sw_frame->data[0] + row * sw_frame->linesize[0],
                sw_frame->width);
        }
        d3d11_ctx_->Unmap(staging_y_texture_, 0);
    }
    else
    {
        result.detail = "Y 平面 Map 失败";
        av_frame_free(&sw_frame);
        return result;
    }

    // ---- 第③步：UV 平面逐行上传 ----
    if (SUCCEEDED(d3d11_ctx_->Map(
        staging_uv_texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        int uv_height = sw_frame->height / 2;
        int uv_width_bytes = sw_frame->width;
        for (int row = 0; row < uv_height; row++)
        {
            memcpy((uint8_t*)mapped.pData + row * mapped.RowPitch,
                sw_frame->data[1] + row * sw_frame->linesize[1],
                uv_width_bytes);
        }
        d3d11_ctx_->Unmap(staging_uv_texture_, 0);
    }
    else
    {
        result.detail = "UV 平面 Map 失败";
        av_frame_free(&sw_frame);
        return result;
    }

    av_frame_free(&sw_frame);

    auto t1 = std::chrono::steady_clock::now();
    result.success = true;
    result.detail = "✅ 下载+上传成功";
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return result;
}

// ================================================================
// 策略 C：CopyResource 到自建 NV12 纹理 → 创建 SRV
// 验证"自己创建一个带 SHADER_RESOURCE 的 NV12 纹理，拷贝数据，能否创建 SRV"
// ================================================================
VideoRenderer::UploadResult VideoRenderer::UploadViaCopiedNV12Texture(AVFrame* frame)
{
    UploadResult result;
    result.strategy_name = "C: CopyResource 到自建 NV12 纹理 → 创建 SRV";

    // ---- 直接取纹理指针 ----
    ID3D11Texture2D* src_texture = (ID3D11Texture2D*)frame->data[0];
    if (!src_texture)
    {
        result.detail = "frame->data[0] 为空";
        return result;
    }

    D3D11_TEXTURE2D_DESC src_desc;
    src_texture->GetDesc(&src_desc);

    // ---- 创建自己的 NV12 纹理（带 SHADER_RESOURCE） ----
    if (!test_nv12_texture_)
    {
        D3D11_TEXTURE2D_DESC my_desc = {};
        my_desc.Width = src_desc.Width;
        my_desc.Height = src_desc.Height;
        my_desc.Format = DXGI_FORMAT_NV12;
        my_desc.MipLevels = 1;
        my_desc.ArraySize = 1;
        my_desc.SampleDesc.Count = 1;
        my_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        my_desc.Usage = D3D11_USAGE_DEFAULT;
        my_desc.MiscFlags = 0;
        my_desc.CPUAccessFlags = 0;

        HRESULT hr = d3d11_device_->CreateTexture2D(&my_desc, nullptr, &test_nv12_texture_);
        if (FAILED(hr))
        {
            result.detail = QString("创建 NV12 纹理失败, HR=0x%1").arg(hr, 0, 16);
            return result;
        }
        result.detail = "创建 NV12 纹理 ✅ | ";
    }
    else
    {
        result.detail = "复用已有 NV12 纹理 | ";
    }

    // ---- CopyResource：GPU 内部拷贝 ----
    d3d11_ctx_->CopyResource(test_nv12_texture_, src_texture);
    result.detail += "CopyResource 完成 | ";

    // ---- 尝试创建 SRV ----
    if (test_nv12_srv_)
    {
        test_nv12_srv_->Release();
        test_nv12_srv_ = nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_NV12;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    HRESULT hr = d3d11_device_->CreateShaderResourceView(
        test_nv12_texture_, &srv_desc, &test_nv12_srv_);
    if (FAILED(hr))
    {
        result.detail += QString("创建 SRV 失败, HR=0x%1").arg(hr, 0, 16);
        return result;
    }

    result.success = true;
    result.detail += "✅ 创建 SRV 成功！";
    return result;
}

// ================================================================
// 策略 D：CopySubresourceRegion + PlaneSlice SRV
// GPU 内部只拷贝一帧 → 两个 SRV 分别指向 Y/UV plane
// 全程不经过 CPU
// ================================================================
VideoRenderer::UploadResult VideoRenderer::UploadViaCopiedNV12Subresource(AVFrame* frame)
{
    UploadResult result;
    result.strategy_name = "D: CopySubresourceRegion → PlaneSlice SRV";

    auto t0 = std::chrono::steady_clock::now();

    // ---- 第①步：从 frame 取解码器纹理和帧索引 ----
    ID3D11Texture2D* decoder_texture = (ID3D11Texture2D*)frame->data[0];
    int subresource_index = (int)(intptr_t)frame->data[1];
    if (!decoder_texture)
    {
        result.detail = "frame->data[0] 为空";
        return result;
    }

    D3D11_TEXTURE2D_DESC dec_desc;
    decoder_texture->GetDesc(&dec_desc);

    int w = dec_desc.Width;
    int h = dec_desc.Height;

    // ---- 第②步：首次运行或分辨率变化时，创建自己的 NV12 纹理和 SRV ----
    if (!nv12_texture_ || w != nv12_width_ || h != nv12_height_)
    {
        // 清理旧资源
        if (nv12_srv_uv_) { nv12_srv_uv_->Release();  nv12_srv_uv_ = nullptr; }
        if (nv12_srv_y_) { nv12_srv_y_->Release();   nv12_srv_y_ = nullptr; }
        if (nv12_texture_) { nv12_texture_->Release(); nv12_texture_ = nullptr; }

        // 创建自己的 NV12 纹理（ArraySize=1，带 SHADER_RESOURCE）
        D3D11_TEXTURE2D_DESC my_desc = {};
        my_desc.Width = w;
        my_desc.Height = h;
        my_desc.Format = DXGI_FORMAT_NV12;
        my_desc.MipLevels = 1;
        my_desc.ArraySize = 1;
        my_desc.SampleDesc.Count = 1;
        my_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        my_desc.Usage = D3D11_USAGE_DEFAULT;
        my_desc.MiscFlags = 0;
        my_desc.CPUAccessFlags = 0;

        HRESULT hr = d3d11_device_->CreateTexture2D(&my_desc, nullptr, &nv12_texture_);
        if (FAILED(hr))
        {
            result.detail = QString("创建 NV12 纹理失败, HR=0x%1").arg(hr, 0, 16);
            return result;
        }

        // ---- 为 NV12 纹理创建两个 SRV（D3D 11.3 PlaneSlice 方案） ----
        D3D11_SHADER_RESOURCE_VIEW_DESC1 srv_desc = {};
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srv_desc.Texture2DArray.MipLevels = 1;
        srv_desc.Texture2DArray.FirstArraySlice = 0;
        srv_desc.Texture2DArray.ArraySize = 1;

        // Y 平面：PlaneSlice=0，格式 R8_UNORM
        srv_desc.Format = DXGI_FORMAT_R8_UNORM;
        srv_desc.Texture2DArray.PlaneSlice = 0;

        ID3D11ShaderResourceView1* tmp_srv_y = nullptr;                  // ← 用 1 版本指针接
        hr = d3d11_device3_->CreateShaderResourceView1(
            nv12_texture_, &srv_desc, &tmp_srv_y);
        if (FAILED(hr))
        {
            result.detail = QString("创建 Y Plane SRV 失败, HR=0x%1").arg(hr, 0, 16);
            return result;
        }
        nv12_srv_y_ = tmp_srv_y;                                         // ← 赋值给老版本指针

        // UV 平面：PlaneSlice=1，格式 R8G8_UNORM
        srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
        srv_desc.Texture2DArray.PlaneSlice = 1;

        ID3D11ShaderResourceView1* tmp_srv_uv = nullptr;
        hr = d3d11_device3_->CreateShaderResourceView1(
            nv12_texture_, &srv_desc, &tmp_srv_uv);
        if (FAILED(hr))
        {
            result.detail = QString("创建 UV Plane SRV 失败, HR=0x%1").arg(hr, 0, 16);
            if (nv12_srv_y_) { nv12_srv_y_->Release(); nv12_srv_y_ = nullptr; }
            return result;
        }
        nv12_srv_uv_ = tmp_srv_uv;

        nv12_width_ = w;
        nv12_height_ = h;

        result.detail = "创建 NV12 纹理 + PlaneSlice SRV ✅ | ";
    }
    else
    {
        result.detail = "复用 NV12 纹理 + SRV | ";
    }

    // ---- 第③步：CopySubresourceRegion — 只拷贝当前帧 ----
    // 源 subresource = 帧索引（因为 MipLevels=1，subresource = index * MipLevels）
    // 目标 subresource = 0（ArraySize=1 的单独纹理）
    D3D11_BOX src_box;
    src_box.left = 0;
    src_box.top = 0;
    src_box.front = 0;
    src_box.right = w;
    src_box.bottom = h;
    src_box.back = 1;

    d3d11_ctx_->CopySubresourceRegion(
        nv12_texture_,                              // 目标纹理
        0,                                          // 目标 subresource
        0, 0, 0,                                    // 目标偏移 (x, y, z)
        decoder_texture,                            // 源纹理
        subresource_index,                          // 源 subresource（= 帧在数组中的索引）
        &src_box);

    auto t1 = std::chrono::steady_clock::now();

    result.success = true;
    result.detail += QString("CopySubresourceRegion ✅ (帧索引=%1)").arg(subresource_index);
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return result;
}

// ================================================================
// 固定渲染管线：不包含上传逻辑，只做 GPU 着色器渲染
// 参数指定用哪两个 SRV（策略 A 用 staging SRV，策略 D 用 NV12 PlaneSlice SRV）
// ================================================================
void VideoRenderer::ExecuteRenderPipeline(
    ID3D11ShaderResourceView* srv_y,
    ID3D11ShaderResourceView* srv_uv)
{
    if (!d3d11_ctx_ || !swapchain_ || !rtv_ || !srv_y || !srv_uv) return;

    // ---- 绑定渲染目标 ----
    d3d11_ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)frame_width_, (float)frame_height_, 0.0f, 1.0f };
    d3d11_ctx_->RSSetViewports(1, &vp);

    // ---- 装配着色器 ----
    d3d11_ctx_->IASetInputLayout(nullptr);
    d3d11_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    d3d11_ctx_->VSSetShader(vertex_shader_, nullptr, 0);
    d3d11_ctx_->PSSetShader(pixel_shader_, nullptr, 0);

    ID3D11ShaderResourceView* srvs[] = { srv_y, srv_uv };
    d3d11_ctx_->PSSetShaderResources(0, 2, srvs);
    d3d11_ctx_->PSSetSamplers(0, 1, &sampler_linear_);

    // ---- Draw + Present ----
    d3d11_ctx_->Draw(3, 0);
    swapchain_->Present(1, 0);

    // ---- 清理 ----
    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
    d3d11_ctx_->PSSetShaderResources(0, 2, null_srvs);
}

// ================================================================
// RenderHardware：数据上传 + 固定渲染
// 上传策略由 active_strategy_ 切换
// ================================================================
void VideoRenderer::RenderHardware(AVFrame* frame)
{
    if (!d3d11_ctx_ || !swapchain_ || !rtv_) return;

    // ---- 第一步：根据当前策略执行数据上传 ----
    UploadResult result;
    ID3D11ShaderResourceView* srv_y = nullptr;
    ID3D11ShaderResourceView* srv_uv = nullptr;

    switch (active_strategy_)
    {
    case UploadStrategy::CPU_TRANSFER:
        result = UploadViaCPUTransfer(frame);
        srv_y = srv_y_;        // 使用 staging 纹理的 SRV
        srv_uv = srv_uv_;
        break;

    case UploadStrategy::COPIED_NV12_PLANE_SLICE:
        result = UploadViaCopiedNV12Subresource(frame);
        srv_y = nv12_srv_y_;   // 使用 NV12 PlaneSlice SRV
        srv_uv = nv12_srv_uv_;
        break;
    }

    // ---- 打印上传结果日志 ----
    //qDebug() << "[UploadStrategy]" << result.strategy_name
    //    << (result.success ? "成功" : "失败")
    //    << "|" << result.detail
    //    << (result.success ? QString("| 耗时 %1ms").arg(result.elapsed_ms, 0, 'f', 1) : "");

    //if (!result.success) return;

    // ---- 第二步：执行固定渲染管线 ----
    ExecuteRenderPipeline(srv_y, srv_uv);
}

// CPU 软解渲染：sws_scale 做 YUV→RGB 转换 → 发射 QImage 到主线程
void VideoRenderer::RenderSoftware(AVFrame* frame)
{
    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame->format);
    int w = frame->width;
    int h = frame->height;

    // 分辨率或格式变化时重新创建 sws 转换上下文
    if (!sws_ctx_ || w != frame_width_ || h != frame_height_)
    {
        qDebug() << "[VideoRenderer] (重新)初始化 SwsContext: 尺寸" << w << "x" << h
            << " 格式:" << src_fmt;
        if (sws_ctx_)
        {
            sws_freeContext(static_cast<SwsContext*>(sws_ctx_));
            sws_ctx_ = nullptr;
        }
        sws_ctx_ = sws_getContext(
            w, h, src_fmt,
            w, h, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        frame_width_ = w;
        frame_height_ = h;

        if (!sws_ctx_)
        {
            qDebug() << "[VideoRenderer] 致命错误: sws_getContext 创建失败！";
            return;
        }
    }
    if (!sws_ctx_) return;

    QImage image(w, h, QImage::Format_RGB888);
    uint8_t* dst[1] = { image.bits() };
    int      dst_stride = static_cast<int>(image.bytesPerLine());

    sws_scale(static_cast<SwsContext*>(sws_ctx_),
        frame->data, frame->linesize, 0, h,
        dst, &dst_stride);

    emit SigFrameReady(image);
}

// 编译内嵌的 HLSL 着色器代码，创建顶点着色器和像素着色器
bool VideoRenderer::InitShaders()
{
    if (!d3d11_device_) return false;

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    // ---- 编译顶点着色器 ----
    D3DCompile(shader_code, strlen(shader_code), nullptr, nullptr,
        nullptr, "VS", "vs_5_0", 0, 0, &vs_blob, &error_blob);
    if (error_blob)
    {
        qDebug() << "VS Compile Error:" << (char*)error_blob->GetBufferPointer();
        error_blob->Release();
        return false;
    }
    d3d11_device_->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_);
    vs_blob->Release();

    // ---- 编译像素着色器 ----
    D3DCompile(shader_code, strlen(shader_code), nullptr, nullptr,
        nullptr, "PS", "ps_5_0", 0, 0, &ps_blob, &error_blob);
    d3d11_device_->CreatePixelShader(
        ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader_);
    ps_blob->Release();

    // ---- 创建线性采样器 ----
    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d3d11_device_->CreateSamplerState(&samp_desc, &sampler_linear_);

    return true;
}