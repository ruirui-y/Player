#include "FFmpegDecoder.h"
#include <QDebug>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

FFmpegDecoder::FFmpegDecoder() {}

FFmpegDecoder::~FFmpegDecoder()
{
    Close();
}

bool FFmpegDecoder::OpenFile(const QString& path, bool try_hardware)
{
    Close();
    qDebug() << "[FFmpegDecoder] 准备打开文件:" << path << "| 尝试硬解:" << try_hardware;

    QByteArray path_bytes = path.toUtf8();
    int ret = avformat_open_input(&fmt_ctx_, path_bytes.constData(), nullptr, nullptr);
    if (ret != 0 || !fmt_ctx_) {
        qDebug() << "[FFmpegDecoder] avformat_open_input 失败, ret =" << ret;
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) { Close(); return false; }

    video_stream_idx_ = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx_ = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_idx_ == -1) {
        qDebug() << "[FFmpegDecoder] 未找到视频流";
        Close();
        return false;
    }

    AVCodecParameters* codecpar = fmt_ctx_->streams[video_stream_idx_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        qDebug() << "[FFmpegDecoder] 未找到对应的解码器";
        Close();
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, codecpar);

    is_hardware_ = false;
    if (try_hardware) {
        AVHWDeviceType hw_type = av_hwdevice_find_type_by_name("d3d11va");
        if (hw_type != AV_HWDEVICE_TYPE_NONE) {
            AVBufferRef* hw_ref = nullptr;
            ret = av_hwdevice_ctx_create(&hw_ref, hw_type, nullptr, nullptr, 0);
            if (ret >= 0 && hw_ref) {
                qDebug() << "[FFmpegDecoder] D3D11VA 硬件设备上下文创建成功";
                ctx->hw_device_ctx = av_buffer_ref(hw_ref);

                // 强制要求现代 D3D11 格式 (AV_PIX_FMT_D3D11)
                ctx->get_format = [](AVCodecContext* c, const AVPixelFormat* fmts) -> AVPixelFormat {
                    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
                        if (*p == AV_PIX_FMT_D3D11) {
                            qDebug() << "[FFmpegDecoder] 格式协商成功: 匹配到 AV_PIX_FMT_D3D11";
                            return *p;
                        }
                    }
                    qDebug() << "[FFmpegDecoder] 格式协商警告: 未找到 AV_PIX_FMT_D3D11，降级为" << fmts[0];
                    return fmts[0];
                    };

                hw_device_ctx_ = hw_ref;
                is_hardware_ = true;
            }
            else {
                qDebug() << "[FFmpegDecoder] D3D11VA 硬件设备上下文创建失败, ret =" << ret;
            }
        }
    }

    ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        qDebug() << "[FFmpegDecoder] 解码器打开失败, ret =" << ret;
        Close();
        return false;
    }

    // 检查硬解是否真正生效
    //if (is_hardware_ && ctx->pix_fmt != AV_PIX_FMT_D3D11) {
    //    qDebug() << "[FFmpegDecoder] 警告: 实际输出格式不是现代 D3D11，硬解回退";
    //    is_hardware_ = false;
    //    av_buffer_unref((AVBufferRef**)(&hw_device_ctx_));
    //    hw_device_ctx_ = nullptr;
    //}

    if (is_hardware_) {
        qDebug() << "[FFmpegDecoder] 解码器就绪: 模式 = D3D11 硬件加速";
    }
    else {
        qDebug() << "[FFmpegDecoder] 解码器就绪: 模式 = 软件解码";
    }

    codec_ctx_ = ctx;
    return true;
}

void* FFmpegDecoder::GetD3D11Device() const
{
    if (!hw_device_ctx_) return nullptr;
    AVHWDeviceContext* dev_ctx = (AVHWDeviceContext*)static_cast<AVBufferRef*>(hw_device_ctx_)->data;
    if (!dev_ctx || !dev_ctx->hwctx) return nullptr;
    void** hwctx_ptr = (void**)dev_ctx->hwctx;
    return hwctx_ptr[0];
}

void FFmpegDecoder::Close()
{
    qDebug() << "[FFmpegDecoder] 释放所有资源";
    if (codec_ctx_) {
        AVCodecContext* p = static_cast<AVCodecContext*>(codec_ctx_);
        avcodec_free_context(&p);
        codec_ctx_ = nullptr;
    }
    if (fmt_ctx_) {
        AVFormatContext* p = static_cast<AVFormatContext*>(fmt_ctx_);
        avformat_close_input(&p);
        fmt_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        AVBufferRef* ref = static_cast<AVBufferRef*>(hw_device_ctx_);
        av_buffer_unref(&ref);
        hw_device_ctx_ = nullptr;
    }
    video_stream_idx_ = -1;
    is_hardware_ = false;
}

int FFmpegDecoder::ReadFrame(AVFrame* frame)
{
    if (!codec_ctx_ || !fmt_ctx_) return -1;
    AVCodecContext* ctx = static_cast<AVCodecContext*>(codec_ctx_);
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_ctx_);

    while (true) {
        AVPacket* pkt = av_packet_alloc();
        int ret = av_read_frame(fmt, pkt);

        if (ret < 0) {
            av_packet_free(&pkt);
            return ret;
        }

        if (pkt->stream_index == video_stream_idx_) {
            ret = avcodec_send_packet(ctx, pkt);
            if (ret < 0) { av_packet_free(&pkt); continue; }

            ret = avcodec_receive_frame(ctx, frame);
            av_packet_free(&pkt);

            if (ret == 0) return 0;
            if (ret == AVERROR(EAGAIN)) continue;
            if (ret == AVERROR_EOF) return ret;
        }
        else {
            av_packet_free(&pkt);
        }
    }
}

bool FFmpegDecoder::Seek(qint64 pos_ms)
{
    if (!fmt_ctx_) return false;
    qDebug() << "[FFmpegDecoder] 执行 Seek，目标时间:" << pos_ms << "ms";
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_ctx_);
    int64_t ts = pos_ms * AV_TIME_BASE / 1000;
    int ret = av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (ret >= 0) FlushBuffers();
    return ret >= 0;
}

void FFmpegDecoder::FlushBuffers()
{
    if (codec_ctx_)
        avcodec_flush_buffers(static_cast<AVCodecContext*>(codec_ctx_));
}

qint64 FFmpegDecoder::GetDuration() const
{
    if (!fmt_ctx_) return 0;
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_ctx_);
    return fmt->duration != AV_NOPTS_VALUE ? fmt->duration * 1000 / AV_TIME_BASE : 0;
}

int FFmpegDecoder::GetWidth() const { return codec_ctx_ ? static_cast<AVCodecContext*>(codec_ctx_)->width : 0; }
int FFmpegDecoder::GetHeight() const { return codec_ctx_ ? static_cast<AVCodecContext*>(codec_ctx_)->height : 0; }
AVRational FFmpegDecoder::GetVideoTimeBase() const {
    if (!fmt_ctx_ || video_stream_idx_ < 0) return { 0, 1 };
    return static_cast<AVFormatContext*>(fmt_ctx_)->streams[video_stream_idx_]->time_base;
}
AVFormatContext* FFmpegDecoder::GetFormatContext() const { return static_cast<AVFormatContext*>(fmt_ctx_); }
AVCodecContext* FFmpegDecoder::GetCodecContext() const { return static_cast<AVCodecContext*>(codec_ctx_); }
bool FFmpegDecoder::IsHardwareDecoding() const { return is_hardware_; }