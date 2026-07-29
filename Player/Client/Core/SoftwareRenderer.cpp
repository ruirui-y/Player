#include "SoftwareRenderer.h"
#include <QDebug>

extern "C"
{
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

SoftwareRenderer::SoftwareRenderer(QObject* parent)
    : QObject(parent)
{
}

SoftwareRenderer::~SoftwareRenderer()
{
    if (sws_ctx_)
    {
        sws_freeContext(static_cast<SwsContext*>(sws_ctx_));
        sws_ctx_ = nullptr;
    }
}

// CPU 软解渲染：sws_scale 做 YUV→RGB 转换 → 发射 QImage 到主线程
void SoftwareRenderer::Render(AVFrame* frame)
{
    if (!frame) return;

    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame->format);
    int w = frame->width;
    int h = frame->height;

    // 分辨率或格式变化时重新创建 sws 转换上下文
    if (!sws_ctx_ || w != frame_width_ || h != frame_height_)
    {
        qDebug() << "[SoftwareRenderer] (重新)初始化 SwsContext: 尺寸" << w << "x" << h
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
            qDebug() << "[SoftwareRenderer] 致命错误: sws_getContext 创建失败！";
            return;
        }
    }
    if (!sws_ctx_) return;

    QImage image(w, h, QImage::Format_RGB888);
    uint8_t* dst[1] = { image.bits() };
    int      dst_stride = static_cast<int>(image.bytesPerLine());

    // ---- YUV→RGB 转换 ----
    sws_scale(static_cast<SwsContext*>(sws_ctx_),
        frame->data, frame->linesize, 0, h,
        dst, &dst_stride);

    // ---- 发射 QImage 到主线程（跨线程信号，Qt 自动用队列连接） ----
    emit SigFrameReady(image);
}
