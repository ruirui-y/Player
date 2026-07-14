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

void VideoRenderer::Release()
{
    d3d11_pipeline_->Release();
    nv12_uploader_->Release();
}

bool VideoRenderer::IsGpuReady() const
{
    return d3d11_pipeline_->IsReady();
}