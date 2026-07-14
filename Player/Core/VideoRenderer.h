#ifndef VIDEORENDERER_H
#define VIDEORENDERER_H

#include <QObject>
#include <QImage>

#include <d3d11.h>
#include <d3d11_3.h>
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

    // ---- 上传策略枚举（测试用） ----
    enum class UploadStrategy
    {
        CPU_TRANSFER,               // A：GPU→CPU 下载 → memcpy 上传（当前方案，最稳）
        COPIED_NV12_TEXTURE,        // C：CopyResource 到自建 NV12 纹理 → 创建 SRV
        COPIED_NV12_PLANE_SLICE,    // D：CopySubresourceRegion → PlaneSlice SRV
    };

    void SetUploadStrategy(UploadStrategy s);                                   // 切换上传策略
    UploadStrategy GetUploadStrategy() const;                                   // 获取当前策略

    // ---- 上传结果（每个策略返回，用于日志对比） ----
    struct UploadResult
    {
        bool        success{ false };                                           // 是否成功
        QString     strategy_name;                                              // 策略名称
        QString     detail;                                                     // 成功/失败的具体原因
        double      elapsed_ms{ 0 };                                            // 耗时
    };

signals:
    void SigFrameReady(const QImage& image);                                    // 软解回退时发射

private:
    // ---- 上传策略函数（每个策略独立实现） ----
    UploadResult UploadViaCPUTransfer(AVFrame* frame);                          // 策略 A：CPU 中转
    UploadResult UploadViaCopiedNV12Texture(AVFrame* frame);                    // 策略 C：CopyResource
    UploadResult UploadViaCopiedNV12Subresource(AVFrame* frame);                // 策略 D
    // ...

    // ---- 固定渲染流程（不包含上传逻辑） ----
    // 前置条件：srv_y_ 和 srv_uv_ 已填充好当前帧的数据
    void ExecuteRenderPipeline(ID3D11ShaderResourceView* srv_y,
        ID3D11ShaderResourceView* srv_uv);

    // ---- 原有私有函数 ----
    bool CreateD3D11SwapChain(ID3D11Device* device, int width, int height);     // 创建交换链
    bool CreateStagingTextures(int width, int height);                          // 创建中转纹理
    bool CreateShaderResourceViews();                                           // 创建着色器资源视图
    void RenderHardware(AVFrame* frame);                                        // GPU 着色器渲染（旧入口，暂时保留）
    void RenderSoftware(AVFrame* frame);                                        // CPU sws_scale 软解渲染

    // ---- 成员变量 ----
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

    // ---- 策略测试用额外纹理（策略 C 需要） ----
    ID3D11Texture2D* test_nv12_texture_{ nullptr };                             // 策略 C：自建 NV12 纹理
    ID3D11ShaderResourceView* test_nv12_srv_{ nullptr };                        // 策略 C：自建 NV12 的 SRV

    // ---- 策略 D：自建 NV12 纹理 + PlaneSlice SRV ----
    ID3D11Device3* d3d11_device3_{ nullptr };
    ID3D11Texture2D* nv12_texture_{ nullptr };                                  // 自建 NV12 纹理
    ID3D11ShaderResourceView* nv12_srv_y_{ nullptr };                           // PlaneSlice=0 → R8 (Y)
    ID3D11ShaderResourceView* nv12_srv_uv_{ nullptr };                          // PlaneSlice=1 → R8G8 (UV)
    int nv12_width_{ 0 };                                                       // 缓存分辨率，变化时重建
    int nv12_height_{ 0 };

    UploadStrategy active_strategy_{ UploadStrategy::COPIED_NV12_PLANE_SLICE };            // 当前选中的上传策略

    bool InitShaders();                                                         // 编译并初始化 HLSL 着色器
};

#endif // VIDEORENDERER_H