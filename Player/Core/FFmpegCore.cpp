#include "FFmpegCore.h"

#include <QImage>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

FFmpegCore::FFmpegCore(QObject* parent)
    : QObject(parent)
{
}

FFmpegCore::~FFmpegCore()
{
    Close();
}

// 打开文件：解封装 → 找视频流 → 打开解码器 → 记录时长
bool FFmpegCore::OpenFile(const QString& path)
{
    // ---- 第一步：打开文件 ----
    QByteArray path_bytes = path.toUtf8();
    AVFormatContext* tmp_ctx = nullptr;
    int ret = avformat_open_input(&tmp_ctx, path_bytes.constData(), nullptr, nullptr);
    if (ret != 0)
    {
        char err_buf[256] = { 0 };
        av_strerror(ret, err_buf, sizeof(err_buf));
        emit SigError(QString::fromUtf8(err_buf));
        return false;
    }
    fmt_ctx_ = tmp_ctx;

    // ---- 第二步：读取流信息 ----
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0)
    {
        emit SigError("avformat_find_stream_info failed");
        Close();
        return false;
    }

    // ---- 第三步：找到视频流 ----
    video_stream_idx_ = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i)
    {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream_idx_ = static_cast<int>(i);
            break;
        }
    }
    if (video_stream_idx_ == -1)
    {
        emit SigError("no video stream found");
        Close();
        return false;
    }

    // ---- 第四步：打开视频解码器 ----
    // ---- 拿到这个视频流的编码参数 ----
    AVCodecParameters* codecpar = fmt_ctx_->streams[video_stream_idx_]->codecpar;

    // ---- 根据编码格式（H264/HEVC等）找到对应的解码器 ----
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        emit SigError("unsupported video codec");
        Close();
        return false;
    }

    // ---- 分配解码器上下文，把参数填进去，再打开解码器 ----
    // 三步连起来就是：领个空壳子 → 塞配置 → 开机
    codec_ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx_, codecpar);

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0)
    {
        emit SigError("avcodec_open2 failed");
        Close();
        return false;
    }
    // ---- 解码器已经就绪，可以往里塞数据包了 ----

    // ---- 第五步：记录视频尺寸和总时长 ----
    frame_width_ = codec_ctx_->width;
    frame_height_ = codec_ctx_->height;

    // 时长单位转换：AV_TIME_BASE 是微秒，转成毫秒
    if (fmt_ctx_->duration != AV_NOPTS_VALUE)
    {
        duration_ms_ = fmt_ctx_->duration * 1000 / AV_TIME_BASE;
    }
    else
    {
        // 某些流式文件没有 duration，用 0 表示未知
        duration_ms_ = 0;
    }

    // ---- 第六步：通知外部加载完成 ----
    emit SigLoaded(duration_ms_);
    return true;
}

// 开始/恢复播放
void FFmpegCore::Play()
{
    if (!fmt_ctx_)
    {
        emit SigError("no file opened");
        return;
    }

    if (playing_)
    {
        // 如果只是暂停中，恢复播放
        if (paused_)
        {
            paused_ = false;
            emit SigPlayState("playing");
        }
        return;
    }

    // 启动解码线程
    playing_ = true;
    paused_ = false;
    decode_thread_ = std::thread(&FFmpegCore::DecodeThreadFunc, this);

    emit SigPlayState("playing");
}

// 暂停
void FFmpegCore::Pause()
{
    if (!playing_)
        return;

    paused_ = true;
    emit SigPlayState("paused");
}

// 停止播放：退出解码线程，回到文件开头
void FFmpegCore::Stop()
{
    if (!playing_)
        return;

    // 让解码线程退出
    playing_ = false;
    paused_ = false;

    if (decode_thread_.joinable())
        decode_thread_.join();

    // 清理解码器内部缓存
    if (codec_ctx_)
        avcodec_flush_buffers(codec_ctx_);

    current_pts_ms_ = 0;
    emit SigPlayState("stopped");
}

// 完全关闭当前文件，释放所有 FFmpeg 资源
void FFmpegCore::Close()
{
    Stop();

    if (sws_ctx_)
    {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (codec_ctx_)
    {
        avcodec_free_context(&codec_ctx_);
    }

    if (fmt_ctx_)
    {
        avformat_close_input(&fmt_ctx_);
    }

    video_stream_idx_ = -1;
    frame_width_ = 0;
    frame_height_ = 0;
    duration_ms_ = 0;
    current_pts_ms_ = 0;
}

qint64 FFmpegCore::GetPosition() const
{
    return current_pts_ms_.load();
}

qint64 FFmpegCore::GetDuration() const
{
    return duration_ms_;
}

bool FFmpegCore::IsPlaying() const
{
    return playing_.load() && !paused_.load();
}

bool FFmpegCore::IsPaused() const
{
    return paused_.load();
}

// ---- 解码线程主循环 ----
void FFmpegCore::DecodeThreadFunc()
{
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    while (playing_)
    {
        // ---- 暂停处理：线程不退出，只是休息 ----
        if (paused_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // ---- 从文件读一个包 ----
        int ret = av_read_frame(fmt_ctx_, packet);
        if (ret < 0)
        {
            // 读完了或出错
            if (ret == AVERROR_EOF)
            {
                emit SigFinished();
            }
            else
            {
                emit SigError("av_read_frame error");
            }
            break;
        }

        // ---- 只处理视频包 ----
        if (packet->stream_index == video_stream_idx_)
        {
            // 送包给解码器
            ret = avcodec_send_packet(codec_ctx_, packet);
            if (ret < 0)
            {
                av_packet_unref(packet);
                continue;
            }

            // 收帧（一个包可能解出多帧）
            while (ret >= 0)
            {
                ret = avcodec_receive_frame(codec_ctx_, frame);
                if (ret == 0)
                {
                    // 计算当前帧的 PTS（毫秒）
                    if (frame->pts != AV_NOPTS_VALUE)
                    {
                        AVRational tb = fmt_ctx_->streams[video_stream_idx_]->time_base;
                        qint64 pts_ms = static_cast<qint64>(
                            frame->pts * av_q2d(tb) * 1000.0);
                        current_pts_ms_ = pts_ms;
                    }

                    // 渲染这一帧
                    RenderFrame(frame);
                }
                else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    break;
                }
            }
        }

        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
}

// ---- 用 sws_scale 将 AVFrame 转成 QImage，然后发射到主线程 ----
void FFmpegCore::RenderFrame(AVFrame* frame)
{
    // 按帧的实际格式创建转换器（只在第一次或分辨率变化时创建）
    AVPixelFormat src_pix_fmt = static_cast<AVPixelFormat>(frame->format);
    int w = frame->width;
    int h = frame->height;

    if (!sws_ctx_
        || w != frame_width_
        || h != frame_height_
        || src_pix_fmt != static_cast<AVPixelFormat>(codec_ctx_->pix_fmt))
    {
        // 旧的转换器如果存在就释放
        if (sws_ctx_)
        {
            sws_freeContext(sws_ctx_);
            sws_ctx_ = nullptr;
        }

        // 创建新的
        sws_ctx_ = sws_getContext(
            w, h, src_pix_fmt,                       // 源格式
            w, h, AV_PIX_FMT_RGB24,                  // 目标格式
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        // 更新缓存的尺寸
        frame_width_ = w;
        frame_height_ = h;
    }

    if (!sws_ctx_)
        return;

    // 分配 RGB 缓冲区
    QImage image(w, h, QImage::Format_RGB888);
    uint8_t* dst_data[1] = { image.bits() };
    int      dst_linesize[1] = { static_cast<int>(image.bytesPerLine()) };

    // 转换
    sws_scale(sws_ctx_,
        frame->data, frame->linesize, 0, h,
        dst_data, dst_linesize);

    // 发射信号到主线程（Qt 自动队列连接，跨线程安全）
    emit SigFrameReady(image);
}