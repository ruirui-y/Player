#ifndef D3D11PIPELINE_H
#define D3D11PIPELINE_H

#include <d3d11.h>
#include <dxgi.h>

// D3D11 渲染管线
// 职责：管理交换链 + HLSL 着色器，提供 Execute() 做 Draw/Present
// 不关心数据从哪来，只负责把两个 SRV（Y + UV）渲染到窗口
class D3D11Pipeline
{
public:
    D3D11Pipeline() = default;
    ~D3D11Pipeline();

    void SetHwnd(HWND hwnd);                                                    // 设置渲染窗口句柄

    ID3D11Device* GetDevice() const;                                            // 返回 D3D11 设备

    bool CreateSwapChain(ID3D11Device* device, int width, int height);          // 用外部设备创建交换链 + 着色器
    void Execute(ID3D11ShaderResourceView* srv_y,                               // 绑定两个 SRV → Draw → Present
        ID3D11ShaderResourceView* srv_uv,
        int width, int height);
    void Release();                                                             // 释放所有资源

    bool IsReady() const;                                                       // 渲染管线是否就绪

    void Clear();                                                               // 清空渲染画面为纯黑

private:
    bool CreateD3D11SwapChain(int width, int height);                           // 创建交换链
    bool InitShaders();                                                         // 编译 HLSL 着色器

    ID3D11Device*           d3d11_device_{ nullptr };                           // D3D11 设备
    ID3D11DeviceContext*    d3d11_ctx_{ nullptr };                              // D3D11 设备上下文
    IDXGISwapChain*         swapchain_{ nullptr };                              // 交换链
    HWND                    hwnd_{ nullptr };                                   // 渲染窗口句柄
    ID3D11RenderTargetView* rtv_{ nullptr };                                    // 渲染目标视图

    ID3D11VertexShader* vertex_shader_{ nullptr };                              // HLSL 顶点着色器
    ID3D11PixelShader*  pixel_shader_{ nullptr };                               // HLSL 像素着色器
    ID3D11SamplerState* sampler_linear_{ nullptr };                             // 线性采样器
};

#endif // D3D11PIPELINE_H
