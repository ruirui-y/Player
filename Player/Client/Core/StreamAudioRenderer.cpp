#include "StreamAudioRenderer.h"

#include <cstring>

#include <QDebug>

#include <mmdeviceapi.h>
#include <audioclient.h>

#pragma comment(lib, "ole32.lib")

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

// ---- 构造 ----
StreamAudioRenderer::StreamAudioRenderer(QObject* parent)
    : QObject(parent)
{
}

// ---- 析构：逆序释放 COM 对象 ----
StreamAudioRenderer::~StreamAudioRenderer()
{
    Close();
}

// ================================================================
// ---- 打开 WASAPI 共享模式渲染设备：枚举器 → 设备 → AudioClient → RenderClient → Start ----
// ================================================================
void StreamAudioRenderer::Start()
{
    // ---- 第一步：创建 swr 重采样上下文（float32-planar → S16 48000Hz 立体声）----
    AVChannelLayout dst_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout src_layout = AV_CHANNEL_LAYOUT_STEREO;

    swr_alloc_set_opts2(&swr_ctx_,
        &dst_layout, AV_SAMPLE_FMT_S16, sample_rate_,                           // 目标：S16 48000Hz 立体声
        &src_layout, AV_SAMPLE_FMT_FLTP, 48000,                                 // 源：float32-planar 48000Hz 立体声
        0, nullptr);

    if (!swr_ctx_ || swr_init(swr_ctx_) < 0)
    {
        qDebug() << "[StreamAudioRenderer] swr_alloc_set_opts 失败";
        return;
    }

    // ---- 第二步：创建设备枚举器 ----
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
    if (FAILED(hr))
    {
        qDebug() << "[StreamAudioRenderer] 创建设备枚举器失败: hr=0x" << Qt::hex << (unsigned)hr;
        return;
    }

    // ---- 第三步：获取默认音频渲染设备（扬声器）----
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr))
    {
        qDebug() << "[StreamAudioRenderer] 获取默认渲染设备失败: hr=0x" << Qt::hex << (unsigned)hr;
        return;
    }

    // ---- 第四步：激活 IAudioClient ----
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client_);
    if (FAILED(hr))
    {
        qDebug() << "[StreamAudioRenderer] 激活 IAudioClient 失败: hr=0x" << Qt::hex << (unsigned)hr;
        return;
    }

    // ---- 第五步：构造 WAVEFORMATEX（16-bit PCM 立体声 48kHz）----
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(channels_);
    wfx.nSamplesPerSec = sample_rate_;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;                 // 4 字节/帧
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    // ---- 第六步：初始化 AudioClient（共享模式）----
    // 40ms 缓冲区（~1920 帧 @ 48kHz），确保至少能容纳 2 帧 Opus（每帧 20ms）
    REFERENCE_TIME hns_buf_duration = 400000;                                    // 40ms 缓冲区
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   0,
                                   hns_buf_duration,
                                   0,
                                   &wfx,
                                   nullptr);
    if (FAILED(hr))
    {
        qDebug() << "[StreamAudioRenderer] Initialize 失败: hr=0x" << Qt::hex << (unsigned)hr;
        return;
    }

    // ---- 第七步：获取硬件缓冲区大小（帧数）----
    hr = audio_client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr))
    {
        qDebug() << "[StreamAudioRenderer] GetBufferSize 失败";
        return;
    }

    // ---- 第八步：获取 IAudioRenderClient（交替写入接口）----
    hr = audio_client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_client_);
    if (FAILED(hr))
    {
        qDebug() << "[StreamAudioRenderer] GetService 失败";
        return;
    }

    // ---- 第九步：启动音频客户端 ----
    hr = audio_client_->Start();
    if (FAILED(hr))
    {
        qDebug() << "[StreamAudioRenderer] Start 失败: hr=0x" << Qt::hex << (unsigned)hr;
        return;
    }

    // ---- 第十步：预先填充静音到整个缓冲区 ----
    // 防止启动瞬间缓冲区为空，声卡播放残留数据导致刺耳噪声
    {
        UINT32 padding = 0;
        audio_client_->GetCurrentPadding(&padding);
        UINT32 free_frames = (buffer_frames_ > padding) ? (buffer_frames_ - padding) : 0;
        if (free_frames > 0)
        {
            BYTE* sil_buf = nullptr;
            hr = render_client_->GetBuffer(free_frames, &sil_buf);
            if (SUCCEEDED(hr))
            {
                std::memset(sil_buf, 0, free_frames * channels_ * sizeof(int16_t));
                render_client_->ReleaseBuffer(free_frames, 0);
                qDebug() << "[StreamAudioRenderer] 已预填充" << free_frames << "帧静音";
            }
        }
    }

    frame_bytes_ = 0;

    qDebug() << "[StreamAudioRenderer] WASAPI 设备已打开:"
             << sample_rate_ << "Hz" << channels_ << "ch"
             << "buffer=" << buffer_frames_ << "frames ("
             << (buffer_frames_ * 1000 / sample_rate_) << "ms)";
}

// 关闭设备，逆序释放 COM 对象
void StreamAudioRenderer::Close()
{
    if (audio_client_)
    {
        audio_client_->Stop();
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (render_client_)
    {
        render_client_->Release();
        render_client_ = nullptr;
    }
    if (device_)
    {
        device_->Release();
        device_ = nullptr;
    }
    if (enumerator_)
    {
        enumerator_->Release();
        enumerator_ = nullptr;
    }

    if (swr_ctx_)
    {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }

    buffer_frames_ = 0;
    frame_bytes_ = 0;
}

// 停止播放
void StreamAudioRenderer::Stop()
{
    Close();
}

// 设置音量（软件音量，在 FeedFrame 中应用到每个采样）
void StreamAudioRenderer::SetVolume(double volume)
{
    volume_ = std::max(0.0, std::min(1.0, volume));
}

// 获取音量
double StreamAudioRenderer::GetVolume() const
{
    return volume_;
}

// 检查 WASAPI 缓冲区能否接受一帧
bool StreamAudioRenderer::CanAcceptFrame() const
{
    if (!audio_client_)
        return false;

    UINT32 padding = 0;
    audio_client_->GetCurrentPadding(&padding);
    UINT32 free_frames = (buffer_frames_ > padding) ? (buffer_frames_ - padding) : 0;
    UINT32 free_bytes = free_frames * channels_ * sizeof(int16_t);
    // 首帧前 frame_bytes_ 为 0，此时不检查帧大小，WASAPI 缓冲区有空间就接受
    if (frame_bytes_ == 0)
        return free_bytes > 0;
    return free_bytes >= (UINT32)frame_bytes_;
}

// 接收已解码的音频帧，swr 重采样后写入 WASAPI 缓冲区
bool StreamAudioRenderer::FeedFrame(AVFrame* frame)
{
    if (!frame || !swr_ctx_ || !render_client_ || !audio_client_)
        return false;

    // ---- 第一步：缓存帧大小（首帧计算，后续复用）----
    if (frame_bytes_ == 0)
    {
        int dst_nb_samples = av_rescale_rnd(
            swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
            sample_rate_, frame->sample_rate, AV_ROUND_UP);
        frame_bytes_ = av_samples_get_buffer_size(
            nullptr, 2, dst_nb_samples, AV_SAMPLE_FMT_S16, 1);
    }

    // ---- 第二步：重采样为 S16 格式 ----
    int dst_nb_samples = av_rescale_rnd(
        swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
        sample_rate_, frame->sample_rate, AV_ROUND_UP);

    QByteArray pcm_data;
    pcm_data.resize(dst_nb_samples * 2 * 2);
    uint8_t* dst[] = { reinterpret_cast<uint8_t*>(pcm_data.data()) };
    int ret = swr_convert(swr_ctx_,
        dst, dst_nb_samples,
        const_cast<const uint8_t**>(frame->data), frame->nb_samples);
    if (ret < 0) return false;

    int actual_size = av_samples_get_buffer_size(nullptr, 2, ret, AV_SAMPLE_FMT_S16, 1);
    int sample_count = actual_size / (2 * sizeof(int16_t));                     // 立体声 = 每帧 4 字节

    // ---- 第三步：软件音量缩放 ----
    if (volume_ < 0.99)
    {
        int16_t* samples = reinterpret_cast<int16_t*>(pcm_data.data());
        int total_samples = actual_size / sizeof(int16_t);
        for (int i = 0; i < total_samples; ++i)
        {
            samples[i] = static_cast<int16_t>(samples[i] * volume_);
        }
    }

    // ---- 第四步：查询 WASAPI 缓冲区空闲空间，写入 PCM ----
    UINT32 padding = 0;
    HRESULT hr = audio_client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) return false;

    UINT32 free_frames = (buffer_frames_ > padding) ? (buffer_frames_ - padding) : 0;
    UINT32 frames_to_write = static_cast<UINT32>(sample_count);
    if (frames_to_write > free_frames)
        frames_to_write = free_frames;
    if (frames_to_write == 0)
        return false;

    BYTE* buffer = nullptr;
    hr = render_client_->GetBuffer(frames_to_write, &buffer);
    if (FAILED(hr)) return false;

    UINT32 bytes_to_copy = frames_to_write * channels_ * sizeof(int16_t);
    std::memcpy(buffer, pcm_data.constData(), bytes_to_copy);

    hr = render_client_->ReleaseBuffer(frames_to_write, 0);
    if (FAILED(hr)) return false;

    return true;
}