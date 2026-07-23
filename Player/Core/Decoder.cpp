// Decoder.cpp
#include "Decoder.h"
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

Decoder::Decoder(SafeQueue<AVPacket*>& packet_queue,
    SafeQueue<AVFrame*>& video_queue,
    SafeQueue<AVFrame*>& audio_queue)
    : packet_queue_(packet_queue)
    , video_queue_(video_queue)
    , audio_queue_(audio_queue)
{
}

Decoder::~Decoder()
{
    Stop();
}

void Decoder::SetStreamIndex(int video_idx, int audio_idx)
{
    video_stream_index_ = video_idx;
    audio_stream_index_ = audio_idx;
}

// ---- 打开视频解码器（支持 D3D11VA 硬解） ----
bool Decoder::OpenVideo(AVCodecParameters* video_par, bool try_hardware)
{
    if (!video_par) return false;

    // ---- 第一步：根据编码格式找到对应的解码器 ----
    const AVCodec* codec = avcodec_find_decoder(video_par->codec_id);
    if (!codec)
    {
        qDebug() << "[Decoder] 未找到视频解码器";
        return false;
    }

    // ---- 第二步：分配解码器上下文，填充编码参数 ----
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, video_par);

    // ---- 第三步：尝试 D3D11VA 硬解初始化 ----
    is_hardware_ = false;
    if (try_hardware)
    {
        AVHWDeviceType hw_type = av_hwdevice_find_type_by_name("d3d11va");
        if (hw_type != AV_HWDEVICE_TYPE_NONE)
        {
            AVBufferRef* hw_ref = nullptr;
            AVDictionary* dict = nullptr;
            av_dict_set(&dict, "debug", "1", 0);
            int ret = av_hwdevice_ctx_create(&hw_ref, hw_type, nullptr, dict, 0);
            if (ret >= 0 && hw_ref)
            {
                qDebug() << "[Decoder] D3D11VA 硬件设备上下文创建成功";
                ctx->hw_device_ctx = av_buffer_ref(hw_ref);

                // get_format 回调：指定只要 AV_PIX_FMT_D3D11
                ctx->get_format = [](AVCodecContext* c,
                    const AVPixelFormat* fmts) -> AVPixelFormat
                    {
                        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
                        {
                            if (*p == AV_PIX_FMT_D3D11)
                            {
                                qDebug() << "[Decoder] 格式协商成功: 匹配到 AV_PIX_FMT_D3D11";
                                return *p;
                            }
                        }
                        qDebug() << "[Decoder] 格式协商警告: 未找到 AV_PIX_FMT_D3D11，降级为" << fmts[0];
                        return fmts[0];
                    };

                hw_device_ctx_ = hw_ref;
                is_hardware_ = true;
            }
            else
            {
                qDebug() << "[Decoder] D3D11VA 硬件设备上下文创建失败, ret =" << ret;
            }
            av_dict_free(&dict);
        }
    }

    // ---- 第四步：打开解码器 ----
    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0)
    {
        qDebug() << "[Decoder] 视频解码器打开失败, ret =" << ret;
        avcodec_free_context(&ctx);
        return false;
    }

    if (is_hardware_)
        qDebug() << "[Decoder] 视频解码器就绪: 模式 = D3D11 硬件加速";
    else
        qDebug() << "[Decoder] 视频解码器就绪: 模式 = 软件解码";

    video_ctx_ = ctx;
    return true;
}

// ---- 打开音频解码器 ----
bool Decoder::OpenAudio(AVCodecParameters* audio_par)
{
    if (!audio_par) return false;

    const AVCodec* codec = avcodec_find_decoder(audio_par->codec_id);
    if (!codec)
    {
        qDebug() << "[Decoder] 未找到音频解码器";
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, audio_par);

    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0)
    {
        qDebug() << "[Decoder] 音频解码器打开失败, ret =" << ret;
        avcodec_free_context(&ctx);
        return false;
    }

    audio_ctx_ = ctx;
    return true;
}

void Decoder::Start()
{
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&Decoder::DecodeLoop, this);
}

void Decoder::Stop()
{
    if (!running_) return;
    running_ = false;
    packet_queue_.Stop();
    if (thread_.joinable())
        thread_.join();

    // 清理硬解设备
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
    if (audio_ctx_)
    {
        avcodec_free_context(&audio_ctx_);
        audio_ctx_ = nullptr;
    }
    video_queue_.Clear();
    audio_queue_.Clear();
}

void Decoder::Flush()
{
    if (video_ctx_) avcodec_flush_buffers(video_ctx_);
    if (audio_ctx_) avcodec_flush_buffers(audio_ctx_);
    video_queue_.Clear();
    audio_queue_.Clear();
}

// ---- 解码线程主循环 ----
void Decoder::DecodeLoop()
{
    qDebug() << "[Decoder] 解码线程启动";

    while (running_)
    {
        AVPacket* pkt = packet_queue_.Pop();
        if (!pkt) break;    // 队列已停止

        if (pkt->stream_index == video_stream_index_)
            DecodeVideoPacket(pkt);
        else if (pkt->stream_index == audio_stream_index_)
            DecodeAudioPacket(pkt);

        qDebug() << "[Decoder] 处理包: stream=" << pkt->stream_index
            << "pts=" << pkt->pts
            << "size=" << pkt->size
            << "| videoQ=" << video_queue_.Size()
            << "audioQ=" << audio_queue_.Size();

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
            video_queue_.Push(frame);
        }
    }
    if (audio_ctx_)
    {
        avcodec_send_packet(audio_ctx_, nullptr);
        while (true)
        {
            AVFrame* frame = av_frame_alloc();
            int ret = avcodec_receive_frame(audio_ctx_, frame);
            if (ret != 0) { av_frame_free(&frame); break; }
            audio_queue_.Push(frame);
        }
    }

    video_queue_.Stop();
    audio_queue_.Stop();
    qDebug() << "[Decoder] 解码线程退出";
}

// ---- 解码一个视频包 ----
void Decoder::DecodeVideoPacket(AVPacket* pkt)
{
    if (!video_ctx_) return;

    int send_ret = avcodec_send_packet(video_ctx_, pkt);
    if (send_ret < 0 && send_ret != AVERROR(EAGAIN))
        return;

    while (true)
    {
        AVFrame* frame = av_frame_alloc();
        int ret = avcodec_receive_frame(video_ctx_, frame);
        if (ret != 0) { av_frame_free(&frame); break; }
        video_queue_.Push(frame);
    }
}

// ---- 解码一个音频包 ----
void Decoder::DecodeAudioPacket(AVPacket* pkt)
{
    if (!audio_ctx_) return;

    int send_ret = avcodec_send_packet(audio_ctx_, pkt);
    if (send_ret < 0 && send_ret != AVERROR(EAGAIN))
        return;

    while (true)
    {
        AVFrame* frame = av_frame_alloc();
        int ret = avcodec_receive_frame(audio_ctx_, frame);
        if (ret != 0) { av_frame_free(&frame); break; }
        audio_queue_.Push(frame);
    }
}

// ---- getter ----
AVCodecContext* Decoder::VideoCodecContext() const { return video_ctx_; }
AVCodecContext* Decoder::AudioCodecContext() const { return audio_ctx_; }
bool Decoder::IsHardwareDecoding() const { return is_hardware_; }

ID3D11Device* Decoder::GetD3D11Device() const
{
    if (!hw_device_ctx_) return nullptr;
    AVHWDeviceContext* dev_ctx = (AVHWDeviceContext*)hw_device_ctx_->data;
    if (!dev_ctx || !dev_ctx->hwctx) return nullptr;
    void** hwctx_ptr = (void**)dev_ctx->hwctx;
    return (ID3D11Device*)hwctx_ptr[0];
}