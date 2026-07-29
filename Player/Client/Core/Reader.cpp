#include "Reader.h"
#include "Common/SafeQueue.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <QDebug>

#define MAX_VIDEO_PACKETS 100
#define MAX_AUDIO_PACKETS 100

Reader::Reader(SafeQueue<AVPacket*>& video_packet_queue,
    SafeQueue<AVPacket*>& audio_packet_queue)
    : video_packet_queue_(video_packet_queue)
    , audio_packet_queue_(audio_packet_queue)
{
}

Reader::~Reader()
{
    Stop();
}

// 打开文件：解封装 → 探测音视频流
bool Reader::Open(const QString& url)
{
    // ---- 第一步：打开文件 ----
    QByteArray path_bytes = url.toUtf8();
    int ret = avformat_open_input(&fmt_ctx_, path_bytes.constData(), nullptr, nullptr);
    if (ret != 0 || !fmt_ctx_)
    {
        qDebug() << "[Reader] avformat_open_input 失败, ret =" << ret;
        return false;
    }

    // ---- 第二步：读取流信息 ----
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0)
    {
        qDebug() << "[Reader] avformat_find_stream_info 失败";
        avformat_close_input(&fmt_ctx_);
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

    // ---- 第四步：找到音频流 ----
    audio_stream_idx_ = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i)
    {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_stream_idx_ = static_cast<int>(i);
            audio_codec_par_ = avcodec_parameters_alloc();
            avcodec_parameters_copy(audio_codec_par_,
                fmt_ctx_->streams[audio_stream_idx_]->codecpar);
            break;
        }
    }

    if (video_stream_idx_ == -1)
    {
        qDebug() << "[Reader] 未找到视频流";
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    qDebug() << "[Reader] 打开成功: 视频流索引 =" << video_stream_idx_
        << ", 音频流索引 =" << audio_stream_idx_;
    return true;
}

void Reader::Start()
{
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&Reader::ReadLoop, this);
}

void Reader::Stop()
{
    if (!running_) return;
    running_ = false;
    video_packet_queue_.Stop();
    audio_packet_queue_.Stop();
    if (thread_.joinable())
        thread_.join();

    if (fmt_ctx_)
    {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    if (audio_codec_par_)
    {
        avcodec_parameters_free(&audio_codec_par_);
        audio_codec_par_ = nullptr;
    }
}

void Reader::Seek(qint64 pts_ms)
{
    seek_target_ms_ = pts_ms;
}

// ---- 读取线程主循环 ----
void Reader::ReadLoop()
{
    qDebug() << "[Reader] 读取线程启动";

    while (running_)
    {
        // ---- 第一步：检查是否需要跳转 ----
        int64_t target_ms = seek_target_ms_.exchange(-1);
        if (target_ms >= 0)
        {
            int64_t ts = target_ms * AV_TIME_BASE / 1000;
            av_seek_frame(fmt_ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);
            qDebug() << "[Reader] 跳转到" << target_ms << "ms";
            video_packet_queue_.Flush();
            audio_packet_queue_.Flush();
        }

        // ---- 第二步：读一个压缩包 ----
        AVPacket* pkt = av_packet_alloc();
        int ret = av_read_frame(fmt_ctx_, pkt);

        if (ret < 0)
        {
            av_packet_free(&pkt);
            qDebug() << "[Reader] 读取结束";

            // ---- 通知两个解码线程文件读取完毕 ----
            video_packet_queue_.Push(nullptr);
            audio_packet_queue_.Push(nullptr);
            qDebug() << "视频压缩队列大小 =" << video_packet_queue_.Size() << ", 音频压缩队列大小 =" << audio_packet_queue_.Size();
            break;
        }

        // ---- 第三步：按流类型分拣到不同队列 ----
        if (pkt->stream_index == video_stream_idx_)
        {
            video_packet_queue_.PushMax(pkt, MAX_VIDEO_PACKETS);
        }
        else if (pkt->stream_index == audio_stream_idx_)
        {
            audio_packet_queue_.PushMax(pkt, MAX_AUDIO_PACKETS);
        }
        else
        {
            av_packet_free(&pkt);    // 字幕等，丢弃
        }
    }

    qDebug() << "[Reader] 读取线程退出";
}

int  Reader::VideoStreamIndex() const { return video_stream_idx_; }
int  Reader::AudioStreamIndex() const { return audio_stream_idx_; }
AVFormatContext* Reader::FormatContext() const { return fmt_ctx_; }
AVCodecParameters* Reader::AudioCodecParameters() const { return audio_codec_par_; }
