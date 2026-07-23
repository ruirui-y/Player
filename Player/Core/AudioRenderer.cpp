#include "AudioRenderer.h"
#include <QDebug>
#include <QThread>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

// ---- 构造 ----
AudioRenderer::AudioRenderer(QObject* parent)
    : QObject(parent)
{
}

AudioRenderer::~AudioRenderer()
{
    Close();
}

// 打开音频解码器
bool AudioRenderer::Open(AVCodecParameters* codecpar)
{
    if (!codecpar)
    {
        qDebug() << "[AudioRenderer] codecpar 为空";
        return false;
    }

    // ---- 第一步：找到并打开音频解码器 ----
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        qDebug() << "[AudioRenderer] 未找到音频解码器";
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx_, codecpar);

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0)
    {
        qDebug() << "[AudioRenderer] 音频解码器打开失败";
        return false;
    }

    qDebug() << "[AudioRenderer] 音频解码器就绪:"
        << "采样率=" << codec_ctx_->sample_rate
        << "声道数=" << codec_ctx_->ch_layout.nb_channels
        << "格式=" << codec_ctx_->sample_fmt;

    return true;
}

// 开始播放
void AudioRenderer::Start()
{
    if (!codec_ctx_) return;

    // ---- 创建重采样上下文（转成 16位 48000Hz 立体声 PCM） ----
    AVChannelLayout dst_layout = AV_CHANNEL_LAYOUT_STEREO;                      // 目标：立体声
    AVChannelLayout src_layout = codec_ctx_->ch_layout;                         // 源：解码器自己的布局

    swr_alloc_set_opts2(&swr_ctx_,
        &dst_layout, AV_SAMPLE_FMT_S16, sample_rate_,                           // 目标：16位 48000Hz 立体声
        &src_layout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate,           // 源：解码器参数
        0, nullptr);

    if (!swr_ctx_ || swr_init(swr_ctx_) < 0)
    {
        qDebug() << "[AudioRenderer] swr_alloc_set_opts 失败";
        return;
    }

    // ---- 创建 QAudioSink（必须在主线程创建） ----
    QAudioFormat format;
    format.setSampleRate(sample_rate_);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    // QAudioSink 必须在主线程创建
    audio_sink_ = new QAudioOutput(format, nullptr);
    audio_io_ = audio_sink_->start();

    playing_ = true;
    paused_ = false;
    total_written_ = 0;

    qDebug() << "[AudioRenderer] 音频播放启动";
}

void AudioRenderer::Pause()
{
    paused_ = true;
    if (audio_sink_) audio_sink_->suspend();
}

void AudioRenderer::Resume()
{
    paused_ = false;
    if (audio_sink_) audio_sink_->resume();
}

void AudioRenderer::Stop()
{
    playing_ = false;
    paused_ = false;

    if (audio_sink_)
    {
        audio_sink_->stop();
        delete audio_sink_;
        audio_sink_ = nullptr;
        audio_io_ = nullptr;
    }
}

void AudioRenderer::Close()
{
    Stop();

    // 清空 PCM 队列
    {
        QMutexLocker lock(&queue_mutex_);
        pcm_queue_.clear();
    }

    if (swr_ctx_)
    {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
    if (codec_ctx_)
    {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    audio_clock_ = 0.0;
    total_written_ = 0;
}

void AudioRenderer::Flush()
{
    QMutexLocker lock(&queue_mutex_);
    pcm_queue_.clear();
}

// 解码一个音频包，返回转换后的 PCM 数据
bool AudioRenderer::DecodePacket(AVPacket* packet)
{
    if (!codec_ctx_ || !playing_ || paused_) return false;

    // ---- 第一步：把压缩包喂给解码器 ----
    int ret = avcodec_send_packet(codec_ctx_, packet);
    if (ret < 0) return false;

    // ---- 第二步：循环收帧，直到解码器说 EAGAIN ----
    AVFrame* frame = av_frame_alloc();
    if (!frame) return false;

    while (ret >= 0)
    {
        ret = avcodec_receive_frame(codec_ctx_, frame);
        if (ret < 0) break;

        // ---- 第三步：重采样为 16位 48000Hz 立体声 PCM ----
        if (!swr_ctx_) break;

        uint8_t* pcm_buffer = nullptr;
        int dst_samples = av_rescale_rnd(
            swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
            sample_rate_, frame->sample_rate, AV_ROUND_UP);

        int dst_buf_size = av_samples_alloc(&pcm_buffer, nullptr,
            2, dst_samples, AV_SAMPLE_FMT_S16, 1);

        int samples_out = swr_convert(swr_ctx_, &pcm_buffer, dst_samples,
            (const uint8_t**)frame->data, frame->nb_samples);

        if (samples_out > 0)
        {
            int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            int pcm_size = samples_out * 2 * bytes_per_sample;

            QByteArray pcm_data((const char*)pcm_buffer, pcm_size);

            // ---- 第四步：把 PCM 数据喂给音频设备 ----
            FeedPcmData(pcm_data);

            // ---- 第五步：更新音频时钟 ----
            // 当前时钟 = 已写入的总样本数 / 采样率 * 1000
            audio_clock_ = (double)total_written_ / (sample_rate_ * 2 * bytes_per_sample) * 1000.0;
        }

        av_freep(&pcm_buffer);
    }

    av_frame_free(&frame);
    return true;
}

// 把 PCM 数据写入音频设备
void AudioRenderer::FeedPcmData(const QByteArray& pcm_data)
{
    if (!audio_io_ || pcm_data.isEmpty()) return;

    qint64 written = audio_io_->write(pcm_data);
    if (written > 0)
    {
        total_written_ += written;
    }
}

// 获取当前音频时钟（毫秒），用于音画同步
double AudioRenderer::GetClock() const
{
    if (audio_sink_)
    {
        qint64 played_us = audio_sink_->processedUSecs();
        return std::max(0.0, played_us / 1000.0);  // ← 确保不为负数
    }
    return audio_clock_;
}

// ---- 接收已解码的音频帧，重采样后播放 ----
// Decoder 已经把压缩包解成了 AVFrame（PCM 数据）
// 这里只需要重采样 + 喂给音频设备，不需要再次 avcodec_send_packet
bool AudioRenderer::FeedFrame(AVFrame* frame)
{
    if (!frame || !swr_ctx_) return false;

    // ---- 第一步：重采样为 S16 格式（QAudioOutput 要求的格式） ----
    int dst_nb_samples = av_rescale_rnd(
        swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
        sample_rate_, frame->sample_rate, AV_ROUND_UP);

    QByteArray pcm_data;
    pcm_data.resize(dst_nb_samples * 2 * 2);  // S16 双声道：每个采样 2 字节 × 2 声道

    uint8_t* dst[] = { reinterpret_cast<uint8_t*>(pcm_data.data()) };

    int ret = swr_convert(swr_ctx_,
        dst, dst_nb_samples,
        const_cast<const uint8_t**>(frame->data), frame->nb_samples);

    if (ret < 0) return false;

    int actual_size = av_samples_get_buffer_size(
        nullptr, 2, ret, AV_SAMPLE_FMT_S16, 1);

    pcm_data.resize(actual_size);

    // ---- 第二步：喂给音频设备 ----
    FeedPcmData(pcm_data);
    return true;
}