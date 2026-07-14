#include "D3D11Pipeline.h"
#include <QDebug>

#include <d3dcompiler.h>

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

D3D11Pipeline::~D3D11Pipeline()
{
    Release();
}

void D3D11Pipeline::SetHwnd(HWND hwnd)
{
    hwnd_ = hwnd;
}

ID3D11Device* D3D11Pipeline::GetDevice() const
{
    return d3d11_device_;
}

bool D3D11Pipeline::IsReady() const
{
    return swapchain_ && pixel_shader_;
}

// 用外部 D3D11 设备创建交换链和着色器
bool D3D11Pipeline::CreateSwapChain(ID3D11Device* device, int width, int height)
{
    Release();
    if (!device || !hwnd_)
    {
        qDebug() << "[D3D11Pipeline] 创建失败: device 或 hwnd_ 为空";
        return false;
    }

    qDebug() << "[D3D11Pipeline] 开始创建渲染管线，分辨率:" << width << "x" << height;

    // 保存设备指针，增加引用计数防止外部意外释放
    d3d11_device_ = device;
    device->AddRef();
    // 获取设备上下文，后续 Map/Unmap/Draw/Present 都靠它
    device->GetImmediateContext(&d3d11_ctx_);

    // ---- 创建交换链（显卡→显示器的桥梁） ----
    if (!CreateD3D11SwapChain(width, height))
    {
        qDebug() << "[D3D11Pipeline] 创建交换链失败";
        return false;
    }

    // ---- 编译 HLSL 着色器（GPU 执行的 NV12→RGB 转换程序） ----
    if (!InitShaders())
    {
        qDebug() << "[D3D11Pipeline] 着色器初始化失败";
        return false;
    }

    qDebug() << "[D3D11Pipeline] 渲染管线全部就绪！";
    return true;
}

// ================================================================
// 创建交换链
// 交换链 = 显卡和显示器之间的"双缓冲画板"
// 显卡往后台画板画，画完后 Present 交换，显示器看前台画板
// ================================================================
bool D3D11Pipeline::CreateD3D11SwapChain(int width, int height)
{
    // ---- 从 D3D11 设备向上查询 DXGI 工厂 ----
    // 设备→适配器（显卡）→工厂（管理交换链的对象）
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;

    d3d11_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev);
    dxgi_dev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);

    // ---- 配置交换链的各项参数 ----
    DXGI_SWAP_CHAIN_DESC scd = {};

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
    scd.Windowed = TRUE;                                            // 窗口模式
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;                 // Win10 推荐的现代翻转模式

    // ---- 让 DXGI 工厂创建交换链 ----
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = factory->CreateSwapChain(d3d11_device_, &scd, &sc);

    // ---- 释放临时接口 ----
    dxgi_dev->Release();
    adapter->Release();
    factory->Release();

    if (FAILED(hr))
    {
        qDebug() << "[D3D11Pipeline] CreateSwapChain 失败，HRESULT:" << hr;
        return false;
    }

    swapchain_ = sc;

    // ---- 创建渲染目标视图 RTV ----
    // 后缓冲是一张纹理，但 GPU 不能直接往纹理里写——需要 RTV 作为"可以画"的许可证
    ID3D11Texture2D* backbuffer = nullptr;
    swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
    d3d11_device_->CreateRenderTargetView(backbuffer, nullptr, &rtv_);
    backbuffer->Release();

    qDebug() << "[D3D11Pipeline] 交换链创建成功";
    return true;
}

// 编译内嵌的 HLSL 着色器代码，创建顶点着色器和像素着色器
bool D3D11Pipeline::InitShaders()
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

// ================================================================
// Execute：绑定渲染目标 → 装配着色器 → Draw → Present
// 这是每帧渲染的核心入口，srv_y/srv_uv 由外部调用者提供
// ================================================================
void D3D11Pipeline::Execute(
    ID3D11ShaderResourceView* srv_y,
    ID3D11ShaderResourceView* srv_uv,
    int width, int height)
{
    if (!d3d11_ctx_ || !swapchain_ || !rtv_ || !srv_y || !srv_uv) return;

    // ---- 第一步：绑定渲染目标 ----
    // RTV 告诉 PS "算出来的 RGB 写到后缓冲这张纹理上"
    d3d11_ctx_->OMSetRenderTargets(1, &rtv_, nullptr);

    // ---- 第二步：设置视口 ----
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    d3d11_ctx_->RSSetViewports(1, &vp);

    // ---- 第三步：装配着色器 ----
    d3d11_ctx_->IASetInputLayout(nullptr);
    d3d11_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    d3d11_ctx_->VSSetShader(vertex_shader_, nullptr, 0);
    d3d11_ctx_->PSSetShader(pixel_shader_, nullptr, 0);

    // 把两个 SRV（Y 和 UV）绑定给像素着色器
    ID3D11ShaderResourceView* srvs[] = { srv_y, srv_uv };
    d3d11_ctx_->PSSetShaderResources(0, 2, srvs);
    d3d11_ctx_->PSSetSamplers(0, 1, &sampler_linear_);

    // ---- 第四步：Draw + Present ----
    // 画一个覆盖全屏的三角形 → 像素着色器对每个像素做 NV12→RGB → Present 上屏
    d3d11_ctx_->Draw(3, 0);
    swapchain_->Present(1, 0);

    // ---- 第五步：清理本帧绑定的 SRV（不影响下一帧） ----
    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
    d3d11_ctx_->PSSetShaderResources(0, 2, null_srvs);
}

// 释放所有 D3D11 资源
void D3D11Pipeline::Release()
{
    if (swapchain_) { swapchain_->Release();       swapchain_ = nullptr; }
    if (d3d11_ctx_) { d3d11_ctx_->Release();       d3d11_ctx_ = nullptr; }
    if (rtv_) { rtv_->Release();               rtv_ = nullptr; }
    if (d3d11_device_) { d3d11_device_->Release();      d3d11_device_ = nullptr; }
    if (vertex_shader_) { vertex_shader_->Release();    vertex_shader_ = nullptr; }
    if (pixel_shader_) { pixel_shader_->Release();     pixel_shader_ = nullptr; }
    if (sampler_linear_) { sampler_linear_->Release();   sampler_linear_ = nullptr; }
    qDebug() << "[D3D11Pipeline] 渲染资源已释放";
}