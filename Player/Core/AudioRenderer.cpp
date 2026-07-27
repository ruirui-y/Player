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

    // ---- [调试] 打开 WAV 文件用于写入原始 PCM ----
    debug_wav_file_.setFileName("debug_audio.pcm");
    debug_wav_file_.open(QIODevice::WriteOnly | QIODevice::Truncate);
    qDebug() << "[AudioRenderer] 调试文件已打开";

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

    if (debug_wav_file_.isOpen())
        debug_wav_file_.close();
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

void AudioRenderer::SetVolume(double volume)
{
    if (!audio_sink_) return;
    volume = std::max(0.0, std::min(1.0, volume));
    audio_sink_->setVolume(volume);
}

double AudioRenderer::GetVolume() const
{
    if (!audio_sink_) return 1.0;
    return audio_sink_->volume();
}

bool AudioRenderer::CanAcceptFrame() const
{
    if (!audio_sink_) return false;
    return audio_sink_->bytesFree() >= frame_bytes_;
}

// 把 PCM 数据写入音频设备
void AudioRenderer::FeedPcmData(const QByteArray& pcm_data)
{
    if (!audio_io_ || pcm_data.isEmpty()) return;

    qint64 free = audio_sink_->bytesFree();
    can_accept_frame_ = (free >= expected_frame_bytes_);

    if (!can_accept_frame_) return;

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

    // 只计算一次，后续帧直接用缓存值
    if (frame_bytes_ == 0)
    {
        int dst_nb_samples = av_rescale_rnd(
            swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
            sample_rate_, frame->sample_rate, AV_ROUND_UP);
        frame_bytes_ = av_samples_get_buffer_size(
            nullptr, 2, dst_nb_samples, AV_SAMPLE_FMT_S16, 1);
        qDebug() << "[AudioRenderer] 帧大小缓存:" << frame_bytes_ << "字节";
    }

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

    if (debug_wav_file_.isOpen())
    {
        debug_wav_file_.write(pcm_data);
    }

    // ---- 第二步：喂给音频设备 ----
    FeedPcmData(pcm_data);
    return true;
}