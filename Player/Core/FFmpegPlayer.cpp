#include "FFmpegPlayer.h"
#include "VideoRenderer.h"
#include "AudioRenderer.h"
#include <QDebug>
#include <chrono>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// ---- 构造 ----
// 创建视频渲染层 + 音频渲染层
FFmpegPlayer::FFmpegPlayer(QObject* parent)
    : QObject(parent)
{
    video_renderer_ = new VideoRenderer(this);
    audio_renderer_ = new AudioRenderer(this);

    // 软解回退时，渲染器的 QImage 信号转发出去给 MainWindow 显示
    QObject::connect(video_renderer_, &VideoRenderer::SigFrameReady,
        this, &FFmpegPlayer::SigFrameReady);
}

// ---- 析构 ----
FFmpegPlayer::~FFmpegPlayer()
{
    Close();
    delete video_renderer_;
    delete audio_renderer_;
}

// ---- 设置渲染窗口句柄 ----
void FFmpegPlayer::SetVideoHwnd(HWND hwnd)
{
    hwnd_ = hwnd;
    video_renderer_->SetHwnd(hwnd);
}

// ---- 打开文件：Reader 解封装 + Decoder 初始化 ----
bool FFmpegPlayer::OpenFile(const QString& path)
{
    Close();

    // ---- 第一步：Reader 打开文件（解封装） ----
    if (!reader_.Open(path))
    {
        emit SigError("OpenFile failed");
        return false;
    }

    // ---- 第二步：取视频宽高和时长 ----
    duration_ms_ = reader_.FormatContext()->duration * 1000 / AV_TIME_BASE;
    AVCodecParameters* video_par =
        reader_.FormatContext()->streams[reader_.VideoStreamIndex()]->codecpar;
    int w = video_par->width;
    int h = video_par->height;

    qDebug() << "[FFmpegPlayer] 视频信息: " << w << "x" << h
        << ", 时长:" << duration_ms_ << "ms";

    // ---- 第三步：尝试硬解打开视频解码器 ----
    if (!decoder_.OpenVideo(video_par, true))
    {
        qDebug() << "[FFmpegPlayer] 硬解失败，尝试软解";
        if (!decoder_.OpenVideo(video_par, false))
        {
            emit SigError("Open video decoder failed");
            return false;
        }
    }

    // ---- 第四步：硬解成功后初始化 GPU 渲染管线 ----
    if (decoder_.IsHardwareDecoding())
    {
        ID3D11Device* device = decoder_.GetD3D11Device();
        if (device)
        {
            if (!video_renderer_->Init(device, w, h))
            {
                qDebug() << "[FFmpegPlayer] 创建 GPU 管线失败，将回退到 CPU 渲染";
            }
        }
    }

    // ---- 第五步：打开音频解码器 + AudioRenderer ----
    if (reader_.AudioStreamIndex() >= 0)
    {
        AVCodecParameters* audio_par = reader_.AudioCodecParameters();
        if (audio_par)
        {
            decoder_.OpenAudio(audio_par);
            audio_renderer_->Open(audio_par);
        }
    }

    // ---- 第六步：告诉 Decoder 音视频流索引 ----
    decoder_.SetStreamIndex(reader_.VideoStreamIndex(), reader_.AudioStreamIndex());

    qDebug() << "[FFmpegPlayer] === 文件加载完毕 ===";
    emit SigLoaded(duration_ms_);
    return true;
}

// ---- 开始/恢复播放 ----
void FFmpegPlayer::Play()
{
    if (playing_)
    {
        if (paused_) { paused_ = false; emit SigPlayState("playing"); }
        return;
    }

    audio_renderer_->Start();

    // 先启动消费者
    playing_ = true;
    paused_ = false;
    decode_thread_ = std::thread(&FFmpegPlayer::DecodeLoop, this);

    // 再启动生产者
    reader_.Start();
    decoder_.Start();

    emit SigPlayState("playing");
}

// ---- 暂停 ----
void FFmpegPlayer::Pause()
{
    if (!playing_) return;
    qDebug() << "[FFmpegPlayer] 暂停播放";
    paused_ = true;
    audio_renderer_->Pause();
    emit SigPlayState("paused");
}

// ---- 停止 ----
void FFmpegPlayer::Stop()
{
    if (!playing_) return;
    qDebug() << "[FFmpegPlayer] 停止播放";
    playing_ = false;

    // 停止读写和解码线程
    reader_.Stop();
    decoder_.Stop();

    if (decode_thread_.joinable()) decode_thread_.join();

    audio_renderer_->Stop();
    current_pts_ms_ = 0;
    emit SigPlayState("stopped");
}

// ---- 关闭全部资源 ----
void FFmpegPlayer::Close()
{
    qDebug() << "[FFmpegPlayer] 关闭播放器引擎";
    Stop();
    reader_.Stop();
    decoder_.Stop();
    video_renderer_->Release();
    audio_renderer_->Close();
    duration_ms_ = 0;
}

// ---- getter ----
qint64 FFmpegPlayer::GetPosition() const { return current_pts_ms_.load(); }
qint64 FFmpegPlayer::GetDuration() const { return duration_ms_; }
bool FFmpegPlayer::IsPlaying() const { return playing_.load() && !paused_.load(); }
bool FFmpegPlayer::IsPaused() const { return paused_.load(); }

// ---- 解码线程主循环 ----
// Reader 线程负责读压缩包 → Decoder 线程负责解压缩
// 本线程（DecodeLoop）只负责：从视频队列取帧 → 同步 → 渲染
void FFmpegPlayer::DecodeLoop()
{
    qDebug() << "[FFmpegPlayer] DecodeLoop 启动";
    auto start_time = std::chrono::steady_clock::now();

    while (playing_)
    {
        if (paused_) 
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 处理音频
        {
            AVFrame* af = nullptr;
            int n = 0;
            while (audio_queue_.TryPop(af) && n < 5)
            {
                audio_renderer_->FeedFrame(af);
                av_frame_free(&af);
                n++;
            }
        }

        // 取视频帧
        AVFrame* video_frame = video_queue_.Pop();
        if (!video_frame) break;

        // 算 PTS（毫秒）
        qint64 pts_ms = 0;
        if (video_frame->pts != AV_NOPTS_VALUE)
        {
            AVRational tb = reader_.FormatContext()
                ->streams[reader_.VideoStreamIndex()]->time_base;
            pts_ms = static_cast<qint64>(video_frame->pts * av_q2d(tb) * 1000.0);
            current_pts_ms_ = pts_ms;
        }

        // ---- 按 PTS 等时间 ----
        if (pts_ms > 0)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_time).count();

            qint64 wait_ms = pts_ms - elapsed;
            if (wait_ms > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
            }
        }

        // 渲染
        video_renderer_->Render(video_frame);
        av_frame_free(&video_frame);
    }

    // 清理
    {
        AVFrame* f = nullptr;
        while (video_queue_.TryPop(f)) av_frame_free(&f);
        while (audio_queue_.TryPop(f)) av_frame_free(&f);
    }

    emit SigFinished();
}