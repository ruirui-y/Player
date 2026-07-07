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
    explicit VideoRenderer(QObject* parent = nullptr);
    ~VideoRenderer();

    void SetHwnd(HWND hwnd);
    ID3D11Device* InitD3D11(int width, int height);
    void          ReleaseD3D11();

    ID3D11Device* GetD3D11Device() const;
    ID3D11DeviceContext* GetD3D11DeviceContext() const;

    bool CreateSwapChain(ID3D11Device* device, int width, int height);
    void Render(AVFrame* frame);

signals:
    void SigFrameReady(const QImage& image);

private:
    void RenderHardware(AVFrame* frame);
    void RenderSoftware(AVFrame* frame);

    ID3D11Device* d3d11_device_{ nullptr };
    ID3D11DeviceContext* d3d11_ctx_{ nullptr };
    IDXGISwapChain* swapchain_{ nullptr };
    HWND                 hwnd_{ nullptr };

    void* sws_ctx_{ nullptr };
    int   frame_width_{ 0 };
    int   frame_height_{ 0 };
};

#endif // VIDEORENDERER_H