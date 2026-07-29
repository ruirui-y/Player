#include "FFmpegPlayer.h"
#include "VideoRenderer.h"
#include "AudioRenderer.h"
#include <QDebug>
#include <chrono>
#include <windows.h> 

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// ---- 构造 ----
FFmpegPlayer::FFmpegPlayer(QObject* parent)
    : QObject(parent)
{
    video_renderer_ = new VideoRenderer(this);
    audio_renderer_ = new AudioRenderer(this);

    QObject::connect(video_renderer_, &VideoRenderer::SigFrameReady,
        this, &FFmpegPlayer::SigFrameReady);
}

// ---- 析构 ----
FFmpegPlayer::~FFmpegPlayer()
{
    delete video_renderer_;
    delete audio_renderer_;
}

// ---- 设置渲染窗口句柄 ----
void FFmpegPlayer::SetVideoHwnd(HWND hwnd)
{
    hwnd_ = hwnd;
    video_renderer_->SetHwnd(hwnd);
}

// ---- 打开文件 ----
bool FFmpegPlayer::OpenFile(const QString& path)
{
    Close();

    if (!reader_.Open(path))
    {
        emit SigError("OpenFile failed");
        return false;
    }

    duration_ms_ = reader_.FormatContext()->duration * 1000 / AV_TIME_BASE;
    AVCodecParameters* video_par =
        reader_.FormatContext()->streams[reader_.VideoStreamIndex()]->codecpar;
    int w = video_par->width;
    int h = video_par->height;

    qDebug() << "[FFmpegPlayer] 视频信息: " << w << "x" << h
        << ", 时长:" << duration_ms_ << "ms";

    video_decoder_.SetStreamIndex(reader_.VideoStreamIndex());
    if (!video_decoder_.OpenVideo(video_par, true))
    {
        qDebug() << "[FFmpegPlayer] 硬解失败，尝试软解";
        if (!video_decoder_.OpenVideo(video_par, false))
        {
            emit SigError("Open video decoder failed");
            return false;
        }
    }

    if (video_decoder_.IsHardwareDecoding())
    {
        ID3D11Device* device = video_decoder_.GetD3D11Device();
        if (device)
        {
            if (!video_renderer_->Init(device, w, h))
            {
                qDebug() << "[FFmpegPlayer] 创建 GPU 管线失败，将回退到 CPU 渲染";
            }
        }
    }

    if (reader_.AudioStreamIndex() >= 0)
    {
        AVCodecParameters* audio_par = reader_.AudioCodecParameters();
        if (audio_par)
        {
            audio_decoder_.SetStreamIndex(reader_.AudioStreamIndex());
            audio_decoder_.OpenAudio(audio_par);
            audio_renderer_->Open(audio_par);
        }
    }

    qDebug() << "[FFmpegPlayer] === 文件加载完毕 ===";
    emit SigLoaded(duration_ms_);
    return true;
}

// ---- 开始/恢复播放 ----
void FFmpegPlayer::Play()
{
    if (playing_)
    {
        if (paused_)
        {
            // 从暂停恢复：补偿 frame_timer
            double now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            double pause_duration = now_ms - pause_start_time_ms_;
            frame_timer_ms_ += pause_duration;

            audio_renderer_->Resume();

            paused_ = false;
            emit SigPlayState("playing");
        }
        return;
    }

    // ---- 重置所有队列（上一次 Close 后队列是 stopped 状态） ----
    video_packet_queue_.Reset();
    audio_packet_queue_.Reset();
    video_frame_queue_.Reset();
    audio_frame_queue_.Reset();
    
    audio_renderer_->Start();

    playing_ = true;
    paused_ = false;
    decode_thread_ = std::thread(&FFmpegPlayer::DecodeLoop, this);

    reader_.Start();
    video_decoder_.Start();
    audio_decoder_.Start();

    emit SigPlayState("playing");
}

// ---- 暂停 ----
void FFmpegPlayer::Pause()
{
    if (!playing_) return;
    qDebug() << "[FFmpegPlayer] 暂停播放";
    paused_ = true;
    pause_start_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    audio_renderer_->Pause();
    emit SigPlayState("paused");
}

void FFmpegPlayer::Seek(qint64 pts_ms)
{
    if (!playing_) return;

    // 1. 设置 reader 跳转目标（异步，ReadLoop 会在下一次循环执行 seek）
    qDebug() << "当前 video_clock_ms = " << video_clock_ms_ << " [FFmpegPlayer] 跳转至 " << pts_ms << "ms";
    reader_.Seek(pts_ms);

    // 2. 重置帧状态（等待新帧自然流入）
    frame_timer_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    last_frame_pts_ms_ = 0.0;
    video_clock_ms_ = 0.0;
}

// ---- 停止播放（只停止线程，不释放解码器/渲染器资源） ----
void FFmpegPlayer::Stop()
{
    if (!playing_) return;
    qDebug() << "[FFmpegPlayer] 停止播放";
    playing_ = false;

    reader_.Stop();
    video_decoder_.Stop();
    audio_decoder_.Stop();

    if (decode_thread_.joinable())
    {
        auto tid = decode_thread_.get_id();
        if (tid != std::this_thread::get_id())   // 防止线程自己 join 自己
            decode_thread_.join();
    }

    audio_renderer_->Stop();
    current_pts_ms_ = 0;
    emit SigPlayState("stopped");
}

// ---- 关闭全部资源（只调用一次） ----
void FFmpegPlayer::Close()
{
    if (duration_ms_ == 0) return;           // 已经关闭过了
    qDebug() << "[FFmpegPlayer] 关闭播放器引擎";

    Stop();

    // 释放渲染器资源（这一步必须在主线程或 COM 初始化后的线程）
    video_renderer_->Release();
    audio_renderer_->Close();

    // 标记已关闭
    duration_ms_ = 0;

    qDebug() << "[FFmpegPlayer] 播放器资源已全部释放";
}

// ---- getter ----
qint64 FFmpegPlayer::GetPosition() const
{
    // 有音频时优先使用音频时钟（processedUSecs），持续递增，更平稳
    // 暂停时 processedUSecs 停在暂停时刻，恢复后继续递增
    if (audio_renderer_ && audio_renderer_->IsOpened())
    {
        return static_cast<qint64>(audio_renderer_->GetClock());
    }
    // 无音频时回退到视频 PTS
    return current_pts_ms_.load();
}
qint64 FFmpegPlayer::GetDuration() const { return duration_ms_; }
bool FFmpegPlayer::IsPlaying() const { return playing_.load() && !paused_.load(); }
bool FFmpegPlayer::IsPaused() const { return paused_.load(); }
void FFmpegPlayer::SetVolume(double volume) { audio_renderer_->SetVolume(volume); }
double FFmpegPlayer::GetVolume() const { return audio_renderer_->GetVolume(); }

// ================================================================
// ---- 渲染线程主循环（音视频同步核心） ----
// ================================================================
void FFmpegPlayer::DecodeLoop()
{
    // ---- 初始化 COM 公寓（子线程调用 D3D11 需要） ----
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    qDebug() << "[FFmpegPlayer] DecodeLoop 启动";

    // ---- 第一步：获取视频帧率 ----
    double fps = 30.0;
    AVStream* vstream = reader_.FormatContext()->streams[reader_.VideoStreamIndex()];
    if (vstream->avg_frame_rate.den > 0 && vstream->avg_frame_rate.num > 0)
        fps = av_q2d(vstream->avg_frame_rate);
    if (fps <= 0) fps = 30.0;
    double frame_interval_ms = 1000.0 / fps;
    double max_frame_duration_ms = 1000.0;
    qDebug() << "[FFmpegPlayer] 视频帧率：" << fps << "fps" << ", 帧间隔：" << frame_interval_ms << "ms";

    // 获取视频 time_base
    AVRational video_tb = reader_.FormatContext()
        ->streams[reader_.VideoStreamIndex()]->time_base;
    bool has_audio = (reader_.AudioStreamIndex() >= 0);

    // ---- 第二步：等待音视频队列都有数据 ----
    while (playing_)
    {
        if (paused_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        bool audio_ok = !has_audio || audio_frame_queue_.Size() >= 2;
        bool video_ok = video_frame_queue_.Size() >= 1;
        if (audio_ok && video_ok) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // ---- 第三步：初始化 ----
    frame_timer_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();                                   // 当前系统时间
    qDebug() << "[FFmpegPlayer] DecodeLoop 开始，当前系统时间：" << frame_timer_ms_;
    double decode_start_time_ms = frame_timer_ms_;
    frame_drops_early_ = 0;
    frame_drops_late_ = 0;
    last_frame_pts_ms_ = 0.0;
    video_clock_ms_ = 0.0;

    // ---- 第五步：主循环 ----
    while (playing_)
    {
        // ---- 暂停处理 ----
        if (paused_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // ---- 喂音频（非阻塞，先检查空间） ----
        if (has_audio)
        {
            AVFrame* af = nullptr;
            int n = 0;
            // 只在声卡能接受数据时才取帧
            while (audio_renderer_->CanAcceptFrame() &&
                audio_frame_queue_.TryPop(af) && n < 5)
            {
                audio_renderer_->FeedFrame(af);
                av_frame_free(&af);
                n++;
            }
        }

        // ---- 取视频帧（阻塞） ----
        AVFrame* video_frame = video_frame_queue_.Pop();
        if (!video_frame) break;

        // ---- 计算视频帧 PTS（秒→毫秒） ----
        double pts_sec = 0.0;
        if (video_frame->pts != AV_NOPTS_VALUE)
            pts_sec = video_frame->pts * av_q2d(video_tb);                                              // 这一帧应该被显示的时间点 (秒)
        double pts_ms = pts_sec * 1000.0;
        current_pts_ms_ = static_cast<qint64>(pts_ms);
        // qDebug() << "[FFmpegPlayer] 解码视频帧，PTS：" << pts_ms << "ms" << " pts_sec: " << pts_sec;

        // ---- 读音频时钟 ----
        double audio_clk_sec = 0.0;
        if (has_audio)
            audio_clk_sec = audio_renderer_->GetClock() / 1000.0;                                       // 当前音频播放时间点 (秒)
        // qDebug() << "audio_clk_sec = " << audio_clk_sec << "s";

        // diff_sec = 0 表示视频帧与音频播放时间点完全同步
        // diff_sec < 0 表示视频帧比音频播放时间点早
        // diff_sec > 0 表示视频帧比音频播放时间点晚
        double diff_sec = pts_sec - audio_clk_sec;                                                      // 当前视频帧与音频播放时间的差值 (秒)
        // qDebug() << "diff_sec =  " << diff_sec << "s";

        // ---- 早期丢帧 ----
        if (has_audio && diff_sec < -0.5 && video_frame_queue_.Size() > 1)
        {
            qDebug() << "视频帧比音频慢 通过丢帧加速追赶";
            frame_drops_early_++;
            av_frame_free(&video_frame);
            continue;
        }

        // ---- 计算基准 delay ----
        double delay_sec = frame_interval_ms / 1000.0;
        if (pts_sec > last_frame_pts_ms_ / 1000.0 && last_frame_pts_ms_ > 0)
        {
            double interval = pts_sec - last_frame_pts_ms_ / 1000.0;
            if (interval > 0.0 && interval < max_frame_duration_ms / 1000.0)
                delay_sec = interval;
        }

        // ---- 同步校正 ----
        if (has_audio)
            delay_sec = ComputeTargetDelay(delay_sec, diff_sec);

        // ---- 等待至显示时间 ----
        double now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        double target_ms = frame_timer_ms_ + delay_sec * 1000.0;

        if (now_ms < target_ms)
        {
            int sleep_ms = static_cast<int>(target_ms - now_ms);
            if (sleep_ms > 0 && sleep_ms < 1000)
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }

        // ---- 更新 frame_timer ----
        frame_timer_ms_ += delay_sec * 1000.0;

        // ---- 容错追平 ----
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms - frame_timer_ms_ > AV_SYNC_THRESHOLD_MAX * 1000.0)
            frame_timer_ms_ = now_ms;

        // ---- 晚期丢帧 ----
        if (has_audio && diff_sec < -0.2 && video_frame_queue_.Size() > 1)
        {
            qDebug() << "视频帧比音频慢 通过丢帧加速追赶";
            frame_drops_late_++;
            av_frame_free(&video_frame);
            continue;
        }

        // ---- 更新视频时钟 ----
        video_clock_ms_ = pts_ms;
        last_frame_pts_ms_ = pts_ms;

        // ---- [调试] 打印前 5 秒的同步数据 ----
        double elapsed = now_ms - decode_start_time_ms;
        //qDebug().noquote()
        //    << QString("PTS=%1ms  audclk=%2ms  diff=%3ms  delay=%4ms  timer=%5ms  elapsed=%6ms  drops(e=%7,l=%8)")
        //    .arg(pts_ms, 8, 'f', 1)
        //    .arg(audio_clk_sec * 1000.0, 0, 'f', 1)
        //    .arg(diff_sec * 1000.0, 5, 'f', 1)
        //    .arg(delay_sec * 1000.0, 5, 'f', 1)
        //    .arg(frame_timer_ms_, 8, 'f', 1)
        //    .arg(now_ms - decode_start_time_ms, 8, 'f', 1)
        //    .arg(frame_drops_early_)
        //    .arg(frame_drops_late_);

        // ---- 渲染 ----
        video_renderer_->Render(video_frame);
        av_frame_free(&video_frame);
    }

    qDebug() << "[FFmpegPlayer] DecodeLoop 退出, early_drop ="
        << frame_drops_early_ << ", late_drop =" << frame_drops_late_;

    {
        AVFrame* f = nullptr;
        while (video_frame_queue_.TryPop(f)) av_frame_free(&f);
        while (audio_frame_queue_.TryPop(f)) av_frame_free(&f);
    }

    if (playing_)
    {
        video_renderer_->ClearFrame();
        emit SigFinished();
    }
    
    // ---- 释放 COM 公寓 ----
    CoUninitialize();
}
