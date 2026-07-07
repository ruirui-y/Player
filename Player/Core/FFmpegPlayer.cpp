#include "FFmpegPlayer.h"
#include "FFmpegDecoder.h"
#include "VideoRenderer.h"
#include <QDebug>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

FFmpegPlayer::FFmpegPlayer(QObject* parent) : QObject(parent)
{
    decoder_ = new FFmpegDecoder();
    renderer_ = new VideoRenderer(this);
    QObject::connect(renderer_, &VideoRenderer::SigFrameReady, this, &FFmpegPlayer::SigFrameReady);
}

FFmpegPlayer::~FFmpegPlayer()
{
    Close();
    delete decoder_;
}

void FFmpegPlayer::SetVideoHwnd(HWND hwnd) { hwnd_ = hwnd; renderer_->SetHwnd(hwnd); }

bool FFmpegPlayer::OpenFile(const QString& path)
{
    Stop();
    qDebug() << "\n[FFmpegPlayer] === 开始加载文件 ===";

    if (!decoder_->OpenFile(path, true)) {
        qDebug() << "[FFmpegPlayer] 硬解启动失败，尝试强制软解模式回退";
        decoder_->Close();
        if (!decoder_->OpenFile(path, false)) {
            qDebug() << "[FFmpegPlayer] 文件打开彻底失败";
            emit SigError("OpenFile failed");
            return false;
        }
    }

    duration_ms_ = decoder_->GetDuration();
    int w = decoder_->GetWidth();
    int h = decoder_->GetHeight();

    qDebug() << "[FFmpegPlayer] 视频信息获取成功 -> 尺寸:" << w << "x" << h << ", 时长:" << duration_ms_ << "ms";

    if (decoder_->IsHardwareDecoding()) {
        ID3D11Device* device = (ID3D11Device*)decoder_->GetD3D11Device();
        if (device) {
            renderer_->CreateSwapChain(device, w, h);
        }
    }

    qDebug() << "[FFmpegPlayer] === 文件加载完毕，等待 Play指令 ===";
    emit SigLoaded(duration_ms_);
    return true;
}

void FFmpegPlayer::Play()
{
    if (!decoder_->GetFormatContext()) { emit SigError("no file opened"); return; }
    if (playing_) {
        if (paused_) {
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

void FFmpegPlayer::DecodeLoop()
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) return;

    AVFormatContext* fmt = decoder_->GetFormatContext();
    AVRational time_base = decoder_->GetVideoTimeBase();
    int frame_count = 0;

    // 假设视频大约是 30 FPS，每帧间隔约 33 毫秒
    // 我们给一个粗略的延时，防止 GUI 线程被事件轰炸卡死
    const int target_frame_delay_ms = 33;

    while (playing_) {
        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 记录解这帧之前的系统时间
        auto start_time = std::chrono::steady_clock::now();

        av_frame_unref(frame);
        int ret = decoder_->ReadFrame(frame);

        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                emit SigFinished();
            }
            break;
        }

        frame_count++;

        if (frame->pts != AV_NOPTS_VALUE && fmt) {
            qint64 pts_ms = static_cast<qint64>(frame->pts * av_q2d(time_base) * 1000.0);
            current_pts_ms_ = pts_ms;
        }

        // 渲染（格式转换与发射信号）
        renderer_->Render(frame);

        // 【核心修改】计算耗时并补齐休眠时间，控制帧率
        auto end_time = std::chrono::steady_clock::now();
        int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        int sleep_time = target_frame_delay_ms - elapsed_ms;
        if (sleep_time > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        }
        else {
            // 如果解码+转换已经超过了33ms（比如4K转换太慢），稍微喘口气，别把CPU占满
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    av_frame_free(&frame);
}