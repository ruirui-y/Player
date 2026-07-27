#include "VideoDecoder.h"
#include "SafeQueue.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <d3d11.h>
#include <QDebug>

// ---- 构造 ----
VideoDecoder::VideoDecoder(SafeQueue<AVPacket*>& packet_queue,
    SafeQueue<AVFrame*>& frame_queue)
    : packet_queue_(packet_queue)
    , frame_queue_(frame_queue)
{
}

// ---- 析构 ----
VideoDecoder::~VideoDecoder()
{
    Stop();
}

// ---- 打开视频解码器（支持 D3D11VA 硬解） ----
bool VideoDecoder::OpenVideo(AVCodecParameters* video_par, bool try_hardware)
{
    if (!video_par) return false;

    // ---- 第一步：找到解码器 ----
    const AVCodec* codec = avcodec_find_decoder(video_par->codec_id);
    if (!codec)
    {
        qDebug() << "[VideoDecoder] 未找到视频解码器";
        return false;
    }

    // ---- 第二步：分配上下文 ----
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, video_par);

    // ---- 第三步：尝试 D3D11VA 硬解 ----
    is_hardware_ = false;
    if (try_hardware)
    {
        AVHWDeviceType hw_type = av_hwdevice_find_type_by_name("d3d11va");
        if (hw_type != AV_HWDEVICE_TYPE_NONE)
        {
            AVBufferRef* hw_ref = nullptr;
            AVDictionary* dict = nullptr;
            // av_dict_set(&dict, "debug", "1", 0);
            int ret = av_hwdevice_ctx_create(&hw_ref, hw_type, nullptr, dict, 0);
            if (ret >= 0 && hw_ref)
            {
                qDebug() << "[VideoDecoder] D3D11VA 硬件设备创建成功";
                ctx->hw_device_ctx = av_buffer_ref(hw_ref);

                // get_format 回调
                ctx->get_format = [](AVCodecContext* c,
                    const AVPixelFormat* fmts) -> AVPixelFormat
                    {
                        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
                        {
                            if (*p == AV_PIX_FMT_D3D11)
                            {
                                qDebug() << "[VideoDecoder] 格式协商成功: AV_PIX_FMT_D3D11";
                                return *p;
                            }
                        }
                        return fmts[0];
                    };

                hw_device_ctx_ = hw_ref;
                is_hardware_ = true;
            }
            else
            {
                qDebug() << "[VideoDecoder] D3D11VA 创建失败, ret =" << ret;
            }
            av_dict_free(&dict);
        }
    }

    // ---- 第四步：打开解码器 ----
    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0)
    {
        qDebug() << "[VideoDecoder] 解码器打开失败, ret =" << ret;
        avcodec_free_context(&ctx);
        return false;
    }

    if (is_hardware_)
        qDebug() << "[VideoDecoder] 就绪: D3D11 硬件加速";
    else
        qDebug() << "[VideoDecoder] 就绪: 软件解码";

    video_ctx_ = ctx;
    return true;
}

// ---- 启动解码线程 ----
void VideoDecoder::Start()
{
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&VideoDecoder::VideoDecodeLoop, this);
}

// ---- 停止解码线程 ----
void VideoDecoder::Stop()
{
    if (!running_) return;
    running_ = false;
    packet_queue_.Stop();          // 解码线程 Pop() 退出
    if (thread_.joinable())
        thread_.join();

    // 清理资源
    if (hw_device_ctx_)
    {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    if (video_ctx_)
    {
        avcodec_free_context(&video_ctx_);
        video_ctx_ = nullptr;
    }
    frame_queue_.Clear();
}

// ---- 刷新解码器 ----
void VideoDecoder::Flush()
{
    if (video_ctx_)
    {
        avcodec_flush_buffers(video_ctx_);
    }
    frame_queue_.Clear();
}

// ---- setter ----
void VideoDecoder::SetStreamIndex(int index)
{
    stream_index_ = index;
}

// ---- 解码线程主循环 ----
void VideoDecoder::VideoDecodeLoop()
{
    qDebug() << "[VideoDecoder] 解码线程启动";

    while (running_)
    {
        AVPacket* pkt = packet_queue_.Pop();
        if (!pkt) break;             // 队列已停止

        DecodePacket(pkt);
        av_packet_free(&pkt);
    }

    // ---- flush 阶段 ----
    if (video_ctx_)
    {
        avcodec_send_packet(video_ctx_, nullptr);
        while (true)
        {
            AVFrame* frame = av_frame_alloc();
            int ret = avcodec_receive_frame(video_ctx_, frame);
            if (ret != 0) { av_frame_free(&frame); break; }
            frame_queue_.Push(frame);
        }
    }

    frame_queue_.Stop();
    qDebug() << "[VideoDecoder] 解码线程退出";
}

// ---- 解码单个包 ----
void VideoDecoder::DecodePacket(AVPacket* pkt)
{
    if (!video_ctx_ || !pkt) return;
    if (pkt->stream_index != stream_index_) return;

    int ret = avcodec_send_packet(video_ctx_, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return;

    while (true)
    {
        AVFrame* frame = av_frame_alloc();
        ret = avcodec_receive_frame(video_ctx_, frame);
        if (ret != 0)
        {
            av_frame_free(&frame);
            break;
        }
        frame_queue_.PushMax(frame, 3);
    }
}

// ---- getter ----
AVCodecContext* VideoDecoder::VideoCodecContext() const { return video_ctx_; }

bool VideoDecoder::IsHardwareDecoding() const { return is_hardware_; }

ID3D11Device* VideoDecoder::GetD3D11Device() const
{
    if (!hw_device_ctx_) return nullptr;
    AVHWDeviceContext* dev_ctx = (AVHWDeviceContext*)hw_device_ctx_->data;
    if (!dev_ctx || !dev_ctx->hwctx) return nullptr;
    void** hwctx_ptr = (void**)dev_ctx->hwctx;
    return (ID3D11Device*)hwctx_ptr[0];
}