#include "VideoRenderer.h"
#include "D3D11Pipeline.h"
#include "Nv12GpuUploader.h"
#include "SoftwareRenderer.h"
#include <QDebug>

extern "C"
{
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
}

VideoRenderer::VideoRenderer(QObject* parent)
    : QObject(parent)
{
    d3d11_pipeline_ = new D3D11Pipeline();
    nv12_uploader_ = new Nv12GpuUploader();
    sw_renderer_ = new SoftwareRenderer(this);

    // 软解回退的 QImage 信号转发出去
    QObject::connect(sw_renderer_, &SoftwareRenderer::SigFrameReady,
        this, &VideoRenderer::SigFrameReady);
}

VideoRenderer::~VideoRenderer()
{
    Release();
    delete d3d11_pipeline_;
    delete nv12_uploader_;
}

void VideoRenderer::SetHwnd(HWND hwnd)
{
    d3d11_pipeline_->SetHwnd(hwnd);
}

bool VideoRenderer::Init(ID3D11Device* device, int width, int height)
{
    // 创建交换链 + 着色器
    if (!d3d11_pipeline_->CreateSwapChain(device, width, height))
    {
        qDebug() << "[VideoRenderer] GPU 管线初始化失败，将使用 CPU 软解";
        return false;
    }

    // 初始化 GPU 上传器（从设备获取上下文）
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    nv12_uploader_->Init(device, ctx);

    qDebug() << "[VideoRenderer] GPU 管线就绪";
    return true;
}

// 统一渲染入口：优先 GPU，失败回退 CPU
void VideoRenderer::Render(AVFrame* frame)
{
    if (!frame) return;

    // ---- 路径一：GPU 硬解帧 → GPU 渲染管线 ----
    if (frame->format == AV_PIX_FMT_D3D11 && d3d11_pipeline_->IsReady())
    {
        LogTextureInfoOnce(frame);

        ID3D11ShaderResourceView* srv_y = nullptr;
        ID3D11ShaderResourceView* srv_uv = nullptr;

        if (nv12_uploader_->UploadFrame(frame, srv_y, srv_uv))
        {
            d3d11_pipeline_->Execute(srv_y, srv_uv,
                frame->width, frame->height);
            return;
        }
    }

    // ---- 路径二：GPU 渲染不可用 → CPU 软解回退 ----
    // D3D11 或 DXVA2 硬解帧需要先下载到 CPU 内存
    if (frame->format == AV_PIX_FMT_D3D11 ||
        frame->format == AV_PIX_FMT_D3D11VA_VLD ||
        frame->format == AV_PIX_FMT_DXVA2_VLD)
    {
        AVFrame* sw_frame = av_frame_alloc();
        if (!sw_frame) return;

        int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
        if (ret == 0)
        {
            sw_renderer_->Render(sw_frame);
        }
        av_frame_free(&sw_frame);
    }
    else
    {
        // 纯软解帧，直接渲染
        sw_renderer_->Render(frame);
    }
}

// ---- 首次 D3D11 帧打印纹理信息（仅一次） ----
void VideoRenderer::LogTextureInfoOnce(AVFrame* frame)
{
    if (format_logged_) return;
    format_logged_ = true;

    ID3D11Texture2D* tex = (ID3D11Texture2D*)frame->data[0];
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);

    const char* fmt_str = "Unknown";
    switch (desc.Format)
    {
    case DXGI_FORMAT_NV12: fmt_str = "DXGI_FORMAT_NV12"; break;
    case DXGI_FORMAT_P010: fmt_str = "DXGI_FORMAT_P010"; break;
    case DXGI_FORMAT_YUY2: fmt_str = "DXGI_FORMAT_YUY2"; break;
    case DXGI_FORMAT_AYUV: fmt_str = "DXGI_FORMAT_AYUV"; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: fmt_str = "DXGI_FORMAT_B8G8R8A8_UNORM"; break;
    }

    qDebug() << "[VideoRenderer] D3D11 解码纹理信息:"
             << "format =" << fmt_str
             << "| ArraySize =" << desc.ArraySize
             << "| BindFlags =" << Qt::hex << desc.BindFlags;
    qDebug() << "[VideoRenderer] 分辨率:" << desc.Width << "x" << desc.Height
             << "| AVFrame 格式: AV_PIX_FMT_D3D11 (171)"
             << "| color_range =" << frame->color_range
             << "| color_space =" << frame->colorspace;
}

void VideoRenderer::Release()
{
    d3d11_pipeline_->Release();
    nv12_uploader_->Release();
}

bool VideoRenderer::IsGpuReady() const
{
    return d3d11_pipeline_->IsReady();
}

void VideoRenderer::ClearFrame()
{
    if (d3d11_pipeline_->IsReady())
        d3d11_pipeline_->Clear();
    // GPU 不可用时（CPU 回退）走 QLabel 清空
}