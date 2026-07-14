#include "FFmpegPlayer.h"
#include "FFmpegDecoder.h"
#include "VideoRenderer.h"
#include <QDebug>
#include <chrono>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

FFmpegPlayer::FFmpegPlayer(QObject* parent)
    : QObject(parent)
{
    decoder_ = new FFmpegDecoder();
    renderer_ = new VideoRenderer(this);
    // 软解回退时，渲染器的 QImage 信号转发出去给 MainWindow 显示
    QObject::connect(renderer_, &VideoRenderer::SigFrameReady,
        this, &FFmpegPlayer::SigFrameReady);
}

FFmpegPlayer::~FFmpegPlayer()
{
    Close();
    delete decoder_;
}

void FFmpegPlayer::SetVideoHwnd(HWND hwnd)
{
    hwnd_ = hwnd;
    renderer_->SetHwnd(hwnd);
}

// 打开文件：尝试硬解，如果失败回退软解
bool FFmpegPlayer::OpenFile(const QString& path)
{
    Stop();
    qDebug() << "\n[FFmpegPlayer] === 开始加载文件 ===";

    // ---- 第一步：尝试 D3D11 硬解打开 ----
    if (!decoder_->OpenFile(path, true))
    {
        qDebug() << "[FFmpegPlayer] 硬解启动失败，尝试强制软解模式回退";
        decoder_->Close();
        if (!decoder_->OpenFile(path, false))
        {
            qDebug() << "[FFmpegPlayer] 文件打开彻底失败";
            emit SigError("OpenFile failed");
            return false;
        }
    }

    duration_ms_ = decoder_->GetDuration();
    int w = decoder_->GetWidth();
    int h = decoder_->GetHeight();

    qDebug() << "[FFmpegPlayer] 视频信息获取成功 -> 尺寸:" << w << "x" << h
        << ", 时长:" << duration_ms_ << "ms";

    // ---- 第二步：如果硬解成功，从解码器取出 D3D11 设备创建交换链 ----
    if (decoder_->IsHardwareDecoding())
    {
        ID3D11Device* device = decoder_->GetD3D11Device();
        if (device)
        {
            if (!renderer_->CreateSwapChain(device, w, h))
            {
                qDebug() << "[FFmpegPlayer] 创建交换链失败，将回退到 CPU 渲染";
            }
        }
    }

    qDebug() << "[FFmpegPlayer] === 文件加载完毕，等待 Play指令 ===";
    emit SigLoaded(duration_ms_);
    return true;
}

void FFmpegPlayer::Play()
{
    if (!decoder_->GetFormatContext()) { emit SigError("no file opened"); return; }
    if (playing_)
    {
        if (paused_)
        {
            qDebug() << "[FFmpegPlayer] 恢复播放";
            paused_ = false;
            emit SigPlayState("playing");
        }
        return;
    }

    qDebug() << "[FFmpegPlayer] 发起启动解码线程";
    playing_ = true;
    paused_ = false;
    decode_thread_ = std::thread(&FFmpegPlayer::DecodeLoop, this);
    emit SigPlayState("playing");
}

void FFmpegPlayer::Pause()
{
    if (!playing_) return;
    qDebug() << "[FFmpegPlayer] 暂停播放";
    paused_ = true;
    emit SigPlayState("paused");
}

void FFmpegPlayer::Stop()
{
    if (!playing_) return;
    qDebug() << "[FFmpegPlayer] 停止播放并销毁解码线程";
    playing_ = false;
    paused_ = false;

    if (decode_thread_.joinable()) decode_thread_.join();

    decoder_->FlushBuffers();
    current_pts_ms_ = 0;
    emit SigPlayState("stopped");
}

void FFmpegPlayer::Close()
{
    qDebug() << "[FFmpegPlayer] 关闭播放器引擎";
    Stop();
    decoder_->Close();
    renderer_->ReleaseD3D11();
    duration_ms_ = 0;
}

qint64 FFmpegPlayer::GetPosition() const { return current_pts_ms_.load(); }
qint64 FFmpegPlayer::GetDuration() const { return duration_ms_; }
bool FFmpegPlayer::IsPlaying() const { return playing_.load() && !paused_.load(); }
bool FFmpegPlayer::IsPaused() const { return paused_.load(); }

// 解码线程主循环
void FFmpegPlayer::DecodeLoop()
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) return;

    AVFormatContext* fmt = decoder_->GetFormatContext();
    AVRational time_base = decoder_->GetVideoTimeBase();
    int frame_count = 0;

    // 根据视频实际帧率计算每帧间隔，用于帧率控制
    double fps = decoder_->GetFrameRate();
    int target_frame_delay_ms = (fps > 0) ? (int)(1000.0 / fps) : 33;

    while (playing_)
    {
        // ---- 暂停时休眠，不退出线程 ----
        if (paused_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto start_time = std::chrono::steady_clock::now();

        av_frame_unref(frame);
        int ret = decoder_->ReadFrame(frame);

        // ---- 文件读完或解码出错 ----
        if (ret < 0)
        {
            qDebug() << "DecodeLoop: ReadFrame returned" << ret
                << "AVERROR_EOF =" << AVERROR_EOF
                << "frame_count =" << frame_count;
            if (ret == AVERROR_EOF)
            {
                emit SigFinished();
            }
            break;
        }

        frame_count++;

        // ---- 更新当前播放 PTS ----
        if (frame->pts != AV_NOPTS_VALUE && fmt)
        {
            qint64 pts_ms = static_cast<qint64>(
                frame->pts * av_q2d(time_base) * 1000.0);
            current_pts_ms_ = pts_ms;
        }

        // ---- 渲染当前帧（GPU 或 CPU） ----
        renderer_->Render(frame);

        // ---- 帧率控制：补齐休眠时间 ----
        auto end_time = std::chrono::steady_clock::now();
        int elapsed_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(end_time - start_time).count();

        int sleep_time = target_frame_delay_ms - elapsed_ms;
        if (sleep_time > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        }
        else
        {
            // 解码+渲染超过帧间隔，稍微歇一秒避免 CPU 满负荷
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    av_frame_free(&frame);
}