#include "AudioDecoder.h"
#include "Common/SafeQueue.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <QDebug>

// ---- 构造 ----
AudioDecoder::AudioDecoder(SafeQueue<AVPacket*>& packet_queue,
    SafeQueue<AVFrame*>& frame_queue)
    : packet_queue_(packet_queue)
    , frame_queue_(frame_queue)
{
}

// ---- 析构 ----
AudioDecoder::~AudioDecoder()
{
    Stop();
}

// ---- 打开音频解码器 ----
bool AudioDecoder::OpenAudio(AVCodecParameters* audio_par)
{
    if (!audio_par)
    {
        qDebug() << "[AudioDecoder] audio_par 为空";
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(audio_par->codec_id);
    if (!codec)
    {
        qDebug() << "[AudioDecoder] 未找到音频解码器";
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, audio_par);

    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0)
    {
        qDebug() << "[AudioDecoder] 解码器打开失败, ret =" << ret;
        avcodec_free_context(&ctx);
        return false;
    }

    qDebug() << "[AudioDecoder] 解码器就绪:"
        << "采样率=" << ctx->sample_rate
        << "声道数=" << ctx->ch_layout.nb_channels
        << "格式=" << ctx->sample_fmt;

    audio_ctx_ = ctx;
    return true;
}

// ---- 启动解码线程 ----
void AudioDecoder::Start()
{
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&AudioDecoder::AudioDecodeLoop, this);
}

// ---- 停止解码线程 ----
void AudioDecoder::Stop()
{
    if (!running_) return;
    running_ = false;
    packet_queue_.Stop();          // 解码线程 Pop() 退出
    if (thread_.joinable())
        thread_.join();

    if (audio_ctx_)
    {
        avcodec_free_context(&audio_ctx_);
        audio_ctx_ = nullptr;
    }
    frame_queue_.Clear();
}

// ---- 刷新解码器 ----
void AudioDecoder::Flush()
{
    if (audio_ctx_)
    {
        avcodec_flush_buffers(audio_ctx_);
    }
    frame_queue_.Clear();
}

// ---- setter ----
void AudioDecoder::SetStreamIndex(int index)
{
    stream_index_ = index;
}

// ---- 解码线程主循环 ----
void AudioDecoder::AudioDecodeLoop()
{
    qDebug() << "[AudioDecoder] 解码线程启动";

    while (running_)
    {
        AVPacket* pkt = packet_queue_.Pop();
        if (!pkt) break;             // 队列已停止

        // ---- 检测 serial 变化（seek 后自动 flush） ----
        int cur_serial = packet_queue_.serial();
        if (cur_serial != last_serial_)
        {
            if (audio_ctx_)
                avcodec_flush_buffers(audio_ctx_);
            frame_queue_.Clear();
            last_serial_ = cur_serial;
            qDebug() << "[AudioDecoder] serial 变化，flush 解码器，新 serial =" << cur_serial;
        }

        DecodePacket(pkt);
        av_packet_free(&pkt);
    }

    // ---- flush 阶段 ----
    if (audio_ctx_)
    {
        avcodec_send_packet(audio_ctx_, nullptr);
        while (true)
        {
            AVFrame* frame = av_frame_alloc();
            int ret = avcodec_receive_frame(audio_ctx_, frame);
            if (ret != 0) { av_frame_free(&frame); break; }
            frame_queue_.PushMax(frame, 9);
        }
    }

    frame_queue_.Stop();
    qDebug() << "[AudioDecoder] 解码线程退出";
}

// ---- 解码单个包 ----
void AudioDecoder::DecodePacket(AVPacket* pkt)
{
    if (!audio_ctx_ || !pkt) return;
    if (pkt->stream_index != stream_index_) return;

    int ret = avcodec_send_packet(audio_ctx_, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return;

    while (true)
    {
        AVFrame* frame = av_frame_alloc();
        ret = avcodec_receive_frame(audio_ctx_, frame);
        if (ret != 0)
        {
            av_frame_free(&frame);
            break;
        }
        frame_queue_.Push(frame);
    }
}

// ---- getter ----
AVCodecContext* AudioDecoder::AudioCodecContext() const { return audio_ctx_; }
