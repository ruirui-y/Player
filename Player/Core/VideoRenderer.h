#ifndef VIDEORENDERER_H
#define VIDEORENDERER_H

#include <QObject>
#include <QImage>

#include <d3d11.h>
#include <dxgi.h>

struct AVFrame;

class VideoRenderer : public QObject
{
    Q_OBJECT

public:
    explicit VideoRenderer(QObject* parent = nullptr);                          // 构造
    ~VideoRenderer();                                                           // 析构

    void SetHwnd(HWND hwnd);                                                    // 设置渲染窗口句柄

    ID3D11Device* InitD3D11(int width, int height);                             // 独立创建 D3D11 设备+交换链
    void          ReleaseD3D11();                                               // 释放全部 D3D11 资源

    ID3D11Device* GetD3D11Device() const;                                       // 给解码器做硬解用
    ID3D11DeviceContext* GetD3D11DeviceContext() const;                         // 给解码器做硬解用

    bool CreateSwapChain(ID3D11Device* device, int width, int height);          // 用外部 D3D11 设备创建交换链
    void Render(AVFrame* frame);                                                // 统一渲染入口，自动分派

signals:
    void SigFrameReady(const QImage& image);                                    // 软解回退时发射

private:
    bool CreateD3D11SwapChain(ID3D11Device* device, int width, int height);     // 创建交换链
    bool CreateStagingTextures(int width, int height);                          // 创建中转纹理
    bool CreateShaderResourceViews();                                           // 创建着色器资源视图
    void RenderHardware(AVFrame* frame);                                        // GPU 着色器渲染
    void RenderSoftware(AVFrame* frame);                                        // CPU sws_scale 软解渲染

    ID3D11Device* d3d11_device_{ nullptr };                                     // D3D11 设备
    ID3D11DeviceContext* d3d11_ctx_{ nullptr };                                 // D3D11 设备上下文
    IDXGISwapChain* swapchain_{ nullptr };                                      // 交换链，用于 Present 到窗口
    HWND                 hwnd_{ nullptr };                                      // 渲染窗口句柄
    ID3D11RenderTargetView* rtv_{ nullptr };                                    // 渲染目标视图，创建交换链时一次性创建

    void* sws_ctx_{ nullptr };                                                  // SwsContext*，CPU RGB 转换
    int   frame_width_{ 0 };                                                    // 当前视频宽度
    int   frame_height_{ 0 };                                                   // 当前视频高度

private:
    ID3D11VertexShader* vertex_shader_{ nullptr };                              // HLSL 顶点着色器
    ID3D11PixelShader* pixel_shader_{ nullptr };                                // HLSL 像素着色器
    ID3D11SamplerState* sampler_linear_{ nullptr };                             // 线性采样器

    ID3D11Texture2D* staging_y_texture_{ nullptr };                             // Y 平面中转纹理（R8_UNORM）
    ID3D11Texture2D* staging_uv_texture_{ nullptr };                            // UV 平面中转纹理（R8G8_UNORM）
    ID3D11ShaderResourceView* srv_y_{ nullptr };                                // Y 平面的着色器资源视图
    ID3D11ShaderResourceView* srv_uv_{ nullptr };                               // UV 平面的着色器资源视图

    bool InitShaders();                                                         // 编译并初始化 HLSL 着色器
};

#endif // VIDEORENDERER_H