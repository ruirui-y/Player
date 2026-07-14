#ifndef VIDEORENDERER_H
#define VIDEORENDERER_H

#include <QObject>
#include <QImage>

#include <d3d11.h>

struct AVFrame;
class D3D11Pipeline;
class Nv12GpuUploader;
class SoftwareRenderer;

// 统一渲染接口
// 职责：内部持有 GPU 管线（D3D11Pipeline + Nv12GpuUploader）和 CPU 软解（SoftwareRenderer）
//       对外只暴露 Render(AVFrame*)，自动分派
class VideoRenderer : public QObject
{
    Q_OBJECT

public:
    explicit VideoRenderer(QObject* parent = nullptr);
    ~VideoRenderer();

    void SetHwnd(HWND hwnd);                                                    // 设置渲染窗口句柄
    bool Init(ID3D11Device* device, int width, int height);                     // 初始化 GPU 渲染管线
    void Render(AVFrame* frame);                                                // 渲染一帧，自动分派 GPU/CPU
    void Release();                                                             // 释放所有资源

    bool IsGpuReady() const;                                                    // GPU 管线是否就绪

signals:
    void SigFrameReady(const QImage& image);                                    // 软解回退时发射

private:
    D3D11Pipeline* d3d11_pipeline_{ nullptr };                                  // GPU 渲染管线
    Nv12GpuUploader* nv12_uploader_{ nullptr };                                 // GPU 帧上传器
    SoftwareRenderer* sw_renderer_{ nullptr };                                  // CPU 软解渲染器
};

#endif // VIDEORENDERER_H