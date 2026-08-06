#include "FFmpegPlayer.h"
#include "VideoRenderer.h"
#include "AudioRenderer.h"
#include "StreamAudioRenderer.h"
#include "Common/Input/InputTransport.h"
#include "Common/Network/NetworkStats.h"
#include "RttMeasurer.h"
#include "Pacer.h"
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
    stream_audio_renderer_ = new StreamAudioRenderer(this);

    QObject::connect(video_renderer_, &VideoRenderer::SigFrameReady,
        this, &FFmpegPlayer::SigFrameReady);
}

// ---- 析构 ----
FFmpegPlayer::~FFmpegPlayer()
{
    delete video_renderer_;
    delete audio_renderer_;
    delete stream_audio_renderer_;
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
    closed_ = false;                                       // 标记为已打开
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

// ---- 打开网络串流（低延迟模式）----
// 与 OpenFile 的区别：不经过 Reader/AVFormatContext，直接用 VideoReceiver 收 UDP 包
// 解码器手动创建 H.264 参数，渲染时跳过音视频同步，收到帧立刻渲染
// 分辨率不在此指定：H.264 解码器会从 SPS（Sequence Parameter Set）自动获取实际分辨率
// 渲染器延迟到首帧到达时用帧的实际分辨率初始化
bool FFmpegPlayer::OpenStream(uint16_t video_port, int fps, uint16_t audio_port)
{
    Close();

    is_streaming_ = true;
    stream_fps_ = fps;
    renderer_inited_ = false;
    duration_ms_ = 0;                                      // 串流无固定时长
    closed_ = false;                                       // 标记为已打开

    // ---- 第一步：手动构造 H.264 的 AVCodecParameters ----
    // 串流没有 AVFormatContext，需要手动告诉解码器 codec_id / 像素格式
    // width/height 设为 0：H.264 解码器会从 SPS NAL 中自动解析实际分辨率
    AVCodecParameters* par = avcodec_parameters_alloc();
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->width = 0;                                        // 由 SPS 自动填充
    par->height = 0;                                       // 由 SPS 自动填充
    par->format = AV_PIX_FMT_NV12;                         // NVENC 输出 NV12

    // ---- 第二步：打开视频解码器 ----
    video_decoder_.SetStreamIndex(0);
    if (!video_decoder_.OpenVideo(par, true))
    {
        qDebug() << "[FFmpegPlayer] 硬解失败，尝试软解";
        if (!video_decoder_.OpenVideo(par, false))
        {
            avcodec_parameters_free(&par);
            emit SigError("Open video decoder failed");
            return false;
        }
    }
    avcodec_parameters_free(&par);

    // ---- 第二步B：手动构造 Opus 的 AVCodecParameters 并打开音频解码器 ----
    // 串流音频使用 Opus 编码，参数固定：48kHz / 2ch / float32-planar
    if (audio_port > 0)
    {
        AVCodecParameters* audio_par = avcodec_parameters_alloc();
        audio_par->codec_type = AVMEDIA_TYPE_AUDIO;
        audio_par->codec_id = AV_CODEC_ID_OPUS;
        audio_par->sample_rate = 48000;
        audio_par->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&audio_par->ch_layout, 2);       // 立体声

        audio_decoder_.SetStreamIndex(0);
        if (audio_decoder_.OpenAudio(audio_par))
        {
            stream_audio_renderer_->Start();
        }
        else
        {
            qDebug() << "[FFmpegPlayer] 音频解码器打开失败，仅视频";
        }
        avcodec_parameters_free(&audio_par);
    }

    // ---- 第三步：渲染器延迟初始化 ----
    // 不在这里创建交换链，因为还不知道服务端发来的视频分辨率
    // 等第一帧解码出来后，用 frame->width/height 初始化渲染器
    // 见 DecodeLoop() 串流模式中的延迟初始化逻辑

    // ---- 第四步：创建并初始化 UDP 视频接收器 ----
    video_receiver_ = new VideoReceiver();
    if (!video_receiver_->Init(video_port, &video_packet_queue_))
    {
        qDebug() << "[FFmpegPlayer] VideoReceiver 初始化失败";
        delete video_receiver_;
        video_receiver_ = nullptr;
        is_streaming_ = false;
        emit SigError("VideoReceiver init failed");
        return false;
    }

    // ---- 服务端 IP 回调：首包到达时通知 PlayerApp（替代轮询） ----
    video_receiver_->SetSenderIPCallback([this](const std::string& ip)
    {
        emit SigSenderIPReady(QString::fromStdString(ip));
    });

    // ---- 第四步B：创建并初始化 UDP 音频接收器 ----
    if (audio_port > 0)
    {
        audio_receiver_ = new AudioReceiver();
        if (!audio_receiver_->Init(audio_port, &audio_packet_queue_))
        {
            qDebug() << "[FFmpegPlayer] AudioReceiver 初始化失败，仅视频";
            delete audio_receiver_;
            audio_receiver_ = nullptr;
        }
    }

    // ---- 第八阶段：创建 VSync 帧步调器 ----
    if (!pacer_)
        pacer_ = new Pacer();

    qDebug() << "[FFmpegPlayer] === 串流加载完毕 ==="
             << "fps" << fps << "视频端口" << video_port << "音频端口" << audio_port
             << "（分辨率待首帧自动获取）";
    emit SigLoaded(0);                                     // 串流无时长，发 0
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

    if (is_streaming_)
    {
        stream_audio_renderer_->Start();
        if (pacer_) pacer_->Start();
    }
    else
    {
        audio_renderer_->Start();
    }

    playing_ = true;
    paused_ = false;
    decode_thread_ = std::thread(&FFmpegPlayer::DecodeLoop, this);

    if (is_streaming_)
    {
        // 串流模式：启动 VideoReceiver + AudioReceiver 代替 Reader
        video_receiver_->Start();
        video_decoder_.Start();
        if (audio_receiver_)
        {
            audio_receiver_->Start();
            audio_decoder_.Start();
        }
    }
    else
    {
        reader_.Start();
        video_decoder_.Start();
        audio_decoder_.Start();
    }

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

    if (is_streaming_)
    {
        // 串流模式：停 VideoReceiver + AudioReceiver
        if (video_receiver_) video_receiver_->Stop();
        video_decoder_.Stop();
        if (audio_receiver_) audio_receiver_->Stop();
        audio_decoder_.Stop();
    }
    else
    {
        reader_.Stop();
        video_decoder_.Stop();
        audio_decoder_.Stop();
    }

    if (decode_thread_.joinable())
    {
        auto tid = decode_thread_.get_id();
        if (tid != std::this_thread::get_id())   // 防止线程自己 join 自己
            decode_thread_.join();
    }

    if (is_streaming_)
    {
        stream_audio_renderer_->Stop();
    }
    else
    {
        audio_renderer_->Stop();
    }
    current_pts_ms_ = 0;
    emit SigPlayState("stopped");
}

// ---- 设置控制信道绑定（IDR 请求 + RTT 测量 + 网络统计上报） ----
void FFmpegPlayer::SetupStreamControl(InputTransportClient* input_transport)
{
    if (!video_receiver_ || !input_transport)
        return;

    // ---- 1. 绑定 IDR 请求（Stage 5） ----
    InputTransportClient* transport = input_transport;
    video_receiver_->SetIdrRequestCallback([transport]()
    {
        transport->SendControlMessage("request_idr");
    });

    // ---- 2. 启动 RTT 测量（Stage 6 新增） ----
    rtt_measurer_ = new RttMeasurer();
    rtt_measurer_->SetSendCallback([transport](uint64_t timestamp_ms)
    {
        transport->SendControlMessage(0x03, (const uint8_t*)&timestamp_ms, sizeof(timestamp_ms));
    });
    rtt_measurer_->Start();

    // ---- 3. 绑定网络统计上报（Stage 6 新增） ----
    // 每秒通过控制信道发送 loss_report 到服务端
    RttMeasurer* rtt_ptr = rtt_measurer_;
    video_receiver_->SetStatsCallback([transport, rtt_ptr](const NetworkStats& stats)
    {
        // 填充 RTT
        NetworkStats st = stats;
        st.rtt_ms = rtt_ptr->GetLatestRtt();

        // 序列化为 JSON 并发送
        std::string json = StatsToJson(st);
        transport->SendControlMessage(0x02, (const uint8_t*)json.c_str(), (int)json.size());
    });

    // ---- 4. 接收服务端消息（Pong + BitrateChange） ----
    input_transport->SetMessageHandler([rtt_ptr](uint8_t msg_type,
        const uint8_t* payload, int payload_len)
    {
        if (msg_type == 0x04 && payload_len == 8)           // Pong
        {
            uint64_t timestamp;
            std::memcpy(&timestamp, payload, 8);
            rtt_ptr->OnPongReceived(timestamp);
        }
        // BitrateChange (0x05) 可以后续处理（显示在 UI 上等）
    });

    qDebug() << "[FFmpegPlayer] 控制信道已绑定（IDR + RTT + 统计上报）";
}

// ---- 关闭全部资源（只调用一次） ----
void FFmpegPlayer::Close()
{
    if (closed_) return;                                   // 已关闭，跳过
    qDebug() << "[FFmpegPlayer] 关闭播放器引擎";

    Stop();

    // 释放渲染器资源（这一步必须在主线程或 COM 初始化后的线程）
    video_renderer_->Release();
    audio_renderer_->Close();
    stream_audio_renderer_->Close();

    // 串流模式：释放 VideoReceiver + AudioReceiver + RttMeasurer
    if (video_receiver_)
    {
        delete video_receiver_;
        video_receiver_ = nullptr;
    }
    if (rtt_measurer_)
    {
        rtt_measurer_->Stop();
        delete rtt_measurer_;
        rtt_measurer_ = nullptr;
    }
    if (pacer_)
    {
        pacer_->Stop();
        delete pacer_;
        pacer_ = nullptr;
    }
    if (audio_receiver_)
    {
        delete audio_receiver_;
        audio_receiver_ = nullptr;
    }

    is_streaming_ = false;
    renderer_inited_ = false;

    // 标记已关闭
    duration_ms_ = 0;
    closed_ = true;

    qDebug() << "[FFmpegPlayer] 播放器资源已全部释放";
}

// ---- getter ----
qint64 FFmpegPlayer::GetPosition() const
{
    // 文件模式：有音频时优先使用音频时钟，持续递增，更平稳
    if (!is_streaming_ && audio_renderer_ && audio_renderer_->IsOpened())
    {
        return static_cast<qint64>(audio_renderer_->GetClock());
    }
    // 串流模式或无音频时：回退到视频 PTS
    return current_pts_ms_.load();
}
qint64 FFmpegPlayer::GetDuration() const { return duration_ms_; }
bool FFmpegPlayer::IsPlaying() const { return playing_.load() && !paused_.load(); }
bool FFmpegPlayer::IsPaused() const { return paused_.load(); }

// 获取视频流发送方 IP（从 UDP 包源地址获取）
std::string FFmpegPlayer::GetSenderIP() const
{
    if (video_receiver_)
        return video_receiver_->GetSenderIP();
    return {};
}

void FFmpegPlayer::SetVolume(double volume)
{
    if (is_streaming_)
        stream_audio_renderer_->SetVolume(volume);
    else
        audio_renderer_->SetVolume(volume);
}
double FFmpegPlayer::GetVolume() const
{
    if (is_streaming_)
        return stream_audio_renderer_->GetVolume();
    return audio_renderer_->GetVolume();
}

// ================================================================
// ---- 渲染线程主循环（调度器） ----
// ================================================================
void FFmpegPlayer::DecodeLoop()
{
    // ---- 初始化 COM 公寓（子线程调用 D3D11 需要） ----
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    qDebug() << "[FFmpegPlayer] DecodeLoop 启动";

    // ---- 第一步：获取流信息 ----
    double fps = 30.0;
    double frame_interval_ms = 1000.0 / 30.0;
    AVRational video_tb = { 1, 90000 };
    bool has_audio = false;
    GetStreamInfo(fps, video_tb, has_audio, frame_interval_ms);

    // ---- 第二步：等待队列就绪 ----
    if (!WaitForFrameQueues(has_audio))
    {
        CoUninitialize();
        return;
    }

    // ---- 第三步：初始化同步状态 ----
    InitSyncState();

    // ---- 第四步：进入主循环 ----
    if (is_streaming_)
        ProcessStreamingLoop(has_audio, video_tb);
    else
        ProcessFileLoop(has_audio, video_tb, fps, frame_interval_ms);

    // ---- 第五步：清理 ----
    CleanupFrames();

    qDebug() << "[FFmpegPlayer] DecodeLoop 退出, early_drop ="
        << frame_drops_early_ << ", late_drop =" << frame_drops_late_;

    if (playing_)
    {
        video_renderer_->ClearFrame();
        emit SigFinished();
    }

    // ---- 释放 COM 公寓 ----
    CoUninitialize();
}

// ================================================================
// ---- 获取流信息（帧率、时基、音频标志、帧间隔） ----
// ================================================================
void FFmpegPlayer::GetStreamInfo(double& fps, AVRational& video_tb, bool& has_audio, double& frame_interval_ms)
{
    if (is_streaming_)
    {
        // 串流模式：固定参数，音频取决于是否有 AudioReceiver
        fps = static_cast<double>(stream_fps_);
        has_audio = (audio_receiver_ != nullptr);
    }
    else
    {
        // 文件模式：从 reader 获取流信息
        AVStream* vstream = reader_.FormatContext()->streams[reader_.VideoStreamIndex()];
        if (vstream->avg_frame_rate.den > 0 && vstream->avg_frame_rate.num > 0)
            fps = av_q2d(vstream->avg_frame_rate);
        if (fps <= 0) fps = 30.0;

        video_tb = reader_.FormatContext()
            ->streams[reader_.VideoStreamIndex()]->time_base;
        has_audio = (reader_.AudioStreamIndex() >= 0);
    }

    frame_interval_ms = 1000.0 / fps;

    qDebug() << "[FFmpegPlayer] 视频帧率：" << fps << "fps" << ", 帧间隔：" << frame_interval_ms << "ms"
             << (is_streaming_ ? "[串流模式]" : "[文件模式]");
}

// ================================================================
// ---- 等待帧队列就绪，返回 false 表示退出 ----
// ================================================================
bool FFmpegPlayer::WaitForFrameQueues(bool has_audio)
{
    while (playing_)
    {
        if (paused_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (is_streaming_)
        {
            // 串流模式：只需视频帧队列有数据
            if (video_frame_queue_.Size() >= 1) return true;
        }
        else
        {
            bool audio_ok = !has_audio || audio_frame_queue_.Size() >= 2;
            bool video_ok = video_frame_queue_.Size() >= 1;
            if (audio_ok && video_ok) return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;                // playing_ 变为 false，退出
}

// ================================================================
// ---- 初始化同步计数器 ----
// ================================================================
void FFmpegPlayer::InitSyncState()
{
    frame_timer_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    qDebug() << "[FFmpegPlayer] DecodeLoop 开始，当前系统时间：" << frame_timer_ms_;
    frame_drops_early_ = 0;
    frame_drops_late_ = 0;
    last_frame_pts_ms_ = 0.0;
    video_clock_ms_ = 0.0;
}

// ================================================================
// ---- 串流主循环（喂音频 + 渲染视频，无同步） ----
// ================================================================
void FFmpegPlayer::ProcessStreamingLoop(bool has_audio, AVRational video_tb)
{
    while (playing_)
    {
        // ---- 暂停处理 ----
        if (paused_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // ---- 喂音频（非阻塞，先检查空间）----
        if (has_audio)
        {
            AVFrame* af = nullptr;
            int n = 0;
            while (stream_audio_renderer_->CanAcceptFrame() &&
                audio_frame_queue_.TryPop(af) && n < 5)
            {
                stream_audio_renderer_->FeedFrame(af);
                av_frame_free(&af);
                n++;
            }
        }

        // ---- 取视频帧（阻塞）----
        AVFrame* video_frame = video_frame_queue_.Pop();
        if (!video_frame) break;

        if (video_frame->pts != AV_NOPTS_VALUE)
            current_pts_ms_ = static_cast<qint64>(video_frame->pts * av_q2d(video_tb) * 1000.0);

        // ---- 延迟初始化渲染器：用首帧的实际分辨率创建交换链 ----
        if (!renderer_inited_ && video_decoder_.IsHardwareDecoding())
        {
            ID3D11Device* device = video_decoder_.GetD3D11Device();
            if (device && video_frame->width > 0 && video_frame->height > 0)
            {
                qDebug() << "[FFmpegPlayer] 首帧到达，初始化渲染器:"
                         << video_frame->width << "x" << video_frame->height;
                video_renderer_->Init(device, video_frame->width, video_frame->height);
                renderer_inited_ = true;
            }
        }

        // ---- 第八阶段：帧计数（FPS 统计） ----
        pacer_->OnFramePresented();
        video_renderer_->Render(video_frame);
        av_frame_free(&video_frame);
    }
}

// ================================================================
// ---- 文件播放主循环（音视频同步 + 渲染） ----
// ================================================================
void FFmpegPlayer::ProcessFileLoop(bool has_audio, AVRational video_tb,
                                   double fps, double frame_interval_ms)
{
    const double max_frame_duration_ms = 1000.0;

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
            pts_sec = video_frame->pts * av_q2d(video_tb);
        double pts_ms = pts_sec * 1000.0;
        current_pts_ms_ = static_cast<qint64>(pts_ms);

        // ---- 读音频时钟 ----
        double audio_clk_sec = 0.0;
        if (has_audio)
            audio_clk_sec = audio_renderer_->GetClock() / 1000.0;

        double diff_sec = pts_sec - audio_clk_sec;

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

        // ---- 渲染 ----
        video_renderer_->Render(video_frame);
        av_frame_free(&video_frame);
    }
}

// ================================================================
// ---- 清理残留帧队列 ----
// ================================================================
void FFmpegPlayer::CleanupFrames()
{
    AVFrame* f = nullptr;
    while (video_frame_queue_.TryPop(f)) av_frame_free(&f);
    while (audio_frame_queue_.TryPop(f)) av_frame_free(&f);
}

// ---- 第八阶段：网络统计读取（供 StreamWindow OSD 使用） ----
int FFmpegPlayer::GetReceiveFps() const
{
    return video_receiver_ ? video_receiver_->GetReceiveFps() : 0;
}

int FFmpegPlayer::GetRenderFps() const
{
    return pacer_ ? pacer_->GetRenderFps() : 0;
}

int FFmpegPlayer::GetRttMs() const
{
    return rtt_measurer_ ? rtt_measurer_->GetLatestRtt() : 0;
}