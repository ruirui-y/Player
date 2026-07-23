#include "FFmpegDecoder.h"
#include <QDebug>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <d3d11.h> 

FFmpegDecoder::FFmpegDecoder() {}

FFmpegDecoder::~FFmpegDecoder()
{
    Close();
}

// 打开文件：解封装 → 找视频流 → 初始化硬解 → 打开解码器
bool FFmpegDecoder::OpenFile(const QString& path, bool try_hardware)
{
    Close();
    qDebug() << "[FFmpegDecoder] 准备打开文件:" << path << "| 尝试硬解:" << try_hardware;

    // ---- 第一步：打开文件 ----
    QByteArray path_bytes = path.toUtf8();
    int ret = avformat_open_input(&fmt_ctx_, path_bytes.constData(), nullptr, nullptr);
    if (ret != 0 || !fmt_ctx_)
    {
        qDebug() << "[FFmpegDecoder] avformat_open_input 失败, ret =" << ret;
        return false;
    }

    // ---- 第二步：读取流信息 ----
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) { Close(); return false; }

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
        qDebug() << "[FFmpegDecoder] 未找到视频流";
        Close();
        return false;
    }

    // ---- 找到音频流 ----
    audio_stream_idx_ = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i)
    {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_stream_idx_ = static_cast<int>(i);
            break;
        }
    }

    if (audio_stream_idx_ >= 0)
    {
        // 拷贝一份音频编码参数，AudioRenderer 会用它独立打开解码器
        audio_codec_par_ = avcodec_parameters_alloc();
        avcodec_parameters_copy(audio_codec_par_,
            fmt_ctx_->streams[audio_stream_idx_]->codecpar);
        qDebug() << "[FFmpegDecoder] 找到音频流，索引 =" << audio_stream_idx_;
    }
    else
    {
        qDebug() << "[FFmpegDecoder] 未找到音频流（纯视频文件）";
    }

    // ---- 第四步：根据编码格式找到对应的解码器 ----
    AVCodecParameters* codecpar = fmt_ctx_->streams[video_stream_idx_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        qDebug() << "[FFmpegDecoder] 未找到对应的解码器";
        Close();
        return false;
    }

    // ---- 第五步：分配解码器上下文，填充编码参数 ----
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, codecpar);

    // ---- 第六步：尝试 D3D11VA 硬解初始化 ----
    is_hardware_ = false;
    if (try_hardware)
    {
        AVHWDeviceType hw_type = av_hwdevice_find_type_by_name("d3d11va");
        if (hw_type != AV_HWDEVICE_TYPE_NONE)
        {
            AVBufferRef* hw_ref = nullptr;
            // 让 FFmpeg 自己创建 D3D11 设备，不需要外部传入
            AVDictionary* dict = nullptr;
            av_dict_set(&dict, "debug", "1", 0);                            // 开启 D3D11 debug layer
            ret = av_hwdevice_ctx_create(&hw_ref, hw_type, nullptr, dict, 0);
            if (ret >= 0 && hw_ref)
            {
                qDebug() << "[FFmpegDecoder] D3D11VA 硬件设备上下文创建成功";
                ctx->hw_device_ctx = av_buffer_ref(hw_ref);

                // get_format 回调：解码器打开时会问"你要什么像素格式"
                // 指定只要 AV_PIX_FMT_D3D11，不要 DXVA2
                ctx->get_format = [](AVCodecContext* c,
                    const AVPixelFormat* fmts) -> AVPixelFormat
                    {
                        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
                        {
                            if (*p == AV_PIX_FMT_D3D11)
                            {
                                qDebug() << "[FFmpegDecoder] 格式协商成功: 匹配到 AV_PIX_FMT_D3D11";
                                return *p;
                            }
                        }
                        qDebug() << "[FFmpegDecoder] 格式协商警告: 未找到 AV_PIX_FMT_D3D11，降级为" << fmts[0];
                        return fmts[0];
                    };

                hw_device_ctx_ = hw_ref;
                is_hardware_ = true;
            }
            else
            {
                qDebug() << "[FFmpegDecoder] D3D11VA 硬件设备上下文创建失败, ret =" << ret;
            }
            av_dict_free(&dict);
        }
    }

    // ---- 第七步：打开解码器 ----
    ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0)
    {
        qDebug() << "[FFmpegDecoder] 解码器打开失败, ret =" << ret;
        Close();
        return false;
    }

    if (is_hardware_)
    {
        qDebug() << "[FFmpegDecoder] 解码器就绪: 模式 = D3D11 硬件加速";
    }
    else
    {
        qDebug() << "[FFmpegDecoder] 解码器就绪: 模式 = 软件解码";
    }

    codec_ctx_ = ctx;
    return true;
}

// 从 FFmpeg 内部的硬件设备上下文中取出 ID3D11Device*，给渲染器创建交换链用
ID3D11Device* FFmpegDecoder::GetD3D11Device() const
{
    if (!hw_device_ctx_) return nullptr;
    AVHWDeviceContext* dev_ctx = (AVHWDeviceContext*)hw_device_ctx_->data;
    if (!dev_ctx || !dev_ctx->hwctx) return nullptr;
    // hwctx 的第一个字段就是 ID3D11Device*
    void** hwctx_ptr = (void**)dev_ctx->hwctx;
    return (ID3D11Device*)hwctx_ptr[0];
}

// 释放所有 FFmpeg 资源
void FFmpegDecoder::Close()
{
    qDebug() << "[FFmpegDecoder] 释放所有资源";

    // 清理音频包队列
    //while (!audio_pkt_queue_.isEmpty())
    //{
    //    av_packet_free(&(audio_pkt_queue_.dequeue()));
    //}

    if (codec_ctx_)
    {
        AVCodecContext* p = static_cast<AVCodecContext*>(codec_ctx_);
        avcodec_free_context(&p);
        codec_ctx_ = nullptr;
    }
    if (fmt_ctx_)
    {
        AVFormatContext* p = static_cast<AVFormatContext*>(fmt_ctx_);
        avformat_close_input(&p);
        fmt_ctx_ = nullptr;
    }
    if (hw_device_ctx_)
    {
        AVBufferRef* ref = static_cast<AVBufferRef*>(hw_device_ctx_);
        av_buffer_unref(&ref);
        hw_device_ctx_ = nullptr;
    }
    if (audio_codec_par_)
    {
        avcodec_parameters_free(&audio_codec_par_);
        audio_codec_par_ = nullptr;
    }
    video_stream_idx_ = -1;
    is_hardware_ = false;
}

// 读一包数据，解一帧画面
// 返回 0 = 成功解码一帧，AVERROR_EOF = 全部解完
int FFmpegDecoder::ReadFrame(AVFrame* frame)
{
    if (!codec_ctx_ || !fmt_ctx_) return -1;

    // ---- 第一步：先尝试收帧（解码器可能还有缓存的帧） ----
    int ret = avcodec_receive_frame(codec_ctx_, frame);
    if (ret == 0) return 0;                    // 成功收到一帧
    if (ret == AVERROR_EOF) return ret;         // 解码器已 flush 完毕

    // ---- 第二步：EAGAIN → 解码器需要更多数据包 ----
    while (ret == AVERROR(EAGAIN))
    {
        AVPacket* pkt = av_packet_alloc();
        int read_ret = av_read_frame(fmt_ctx_, pkt);

        if (read_ret < 0)
        {
            // 文件读完了，发送 NULL 包让解码器 flush 缓存
            av_packet_free(&pkt);
            avcodec_send_packet(codec_ctx_, nullptr);
            break;
        }

        // 只处理视频流的包
        if (pkt->stream_index == video_stream_idx_)
        {
            int send_ret = avcodec_send_packet(codec_ctx_, pkt);
            av_packet_free(&pkt);

            // send 成功后尝试收帧（不管 send 是 >=0 还是 EAGAIN）
            if (send_ret >= 0 || send_ret == AVERROR(EAGAIN))
            {
                ret = avcodec_receive_frame(codec_ctx_, frame);
                if (ret == 0) return 0;        // 成功解码一帧
                if (ret == AVERROR_EOF) return ret;
                // ret == EAGAIN → 继续循环读更多包
            }
        }
        // ReadFrame 内部的 else 分支改为：
        if (pkt->stream_index == audio_stream_idx_)
        {
            // 放进队列，不丢
            audio_pkt_queue_.enqueue(pkt);
            continue;                 // 继续读下一个包（我要找视频包）
        }
        else
        {
            av_packet_free(&pkt);     // 字幕等，丢掉
        }
    }

    // ---- 第三步：flush 阶段，吐出解码器剩余的帧 ----
    while (true)
    {
        ret = avcodec_receive_frame(codec_ctx_, frame);
        if (ret == 0) return 0;                // flush 阶段解出一帧
        return AVERROR_EOF;                    // 全部解完，返回 EOF
    }
}

// ---- 统一读包入口 ----
// DecodeLoop 只调用这一个函数，自动分拣音视频
// output_frame:    视频帧就绪时有效
// output_audio_pkt:音频包就绪时有效（调用者负责 av_packet_free）
ReadResult FFmpegDecoder::ReadNext(
    AVFrame* output_frame, AVPacket* output_audio_pkt)
{
    if (!codec_ctx_ || !fmt_ctx_) return ReadResult::ERR;

    // ---- 第一步：先尝试收视频帧（解码器内部可能有缓存帧） ----
    int ret = avcodec_receive_frame(codec_ctx_, output_frame);
    if (ret == 0) return ReadResult::VIDEO_FRAME;
    if (ret == AVERROR_EOF) return ReadResult::EOF_REACHED;

    // ---- 第二步：收不到 → 读包 ----
    while (ret == AVERROR(EAGAIN))
    {
        AVPacket* pkt = av_packet_alloc();
        int read_ret = av_read_frame(fmt_ctx_, pkt);

        if (read_ret < 0)
        {
            // 文件读完了，flush 解码器
            av_packet_free(&pkt);
            avcodec_send_packet(codec_ctx_, nullptr);
            break;
        }

        // ---- 是视频包 → 喂给解码器 ----
        if (pkt->stream_index == video_stream_idx_)
        {
            int send_ret = avcodec_send_packet(codec_ctx_, pkt);
            av_packet_free(&pkt);

            if (send_ret >= 0 || send_ret == AVERROR(EAGAIN))
            {
                ret = avcodec_receive_frame(codec_ctx_, output_frame);
                if (ret == 0) return ReadResult::VIDEO_FRAME;
                if (ret == AVERROR_EOF) return ReadResult::EOF_REACHED;
                // EAGAIN → 继续循环读包
            }
        }
        // ---- 是音频包 → 通过参数返回 ----
        else if (pkt->stream_index == audio_stream_idx_ && output_audio_pkt)
        {
            av_packet_move_ref(output_audio_pkt, pkt);
            av_packet_free(&pkt);
            return ReadResult::AUDIO_PACKET;
        }
        else
        {
            // 其他包（字幕等）→ 丢弃
            av_packet_free(&pkt);
        }
    }

    // ---- 第三步：flush 阶段 ----
    while (true)
    {
        ret = avcodec_receive_frame(codec_ctx_, output_frame);
        if (ret == 0) return ReadResult::VIDEO_FRAME;
        return ReadResult::EOF_REACHED;
    }
}

// 跳转到指定毫秒位置
bool FFmpegDecoder::Seek(qint64 pos_ms)
{
    if (!fmt_ctx_) return false;
    qDebug() << "[FFmpegDecoder] 执行 Seek，目标时间:" << pos_ms << "ms";
    int64_t ts = pos_ms * AV_TIME_BASE / 1000;
    int ret = av_seek_frame(fmt_ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (ret >= 0) FlushBuffers();
    return ret >= 0;
}

// 清空解码器内部缓存（seek 后必须调用，否则解码器状态错乱）
void FFmpegDecoder::FlushBuffers()
{
    if (codec_ctx_)
        avcodec_flush_buffers(codec_ctx_);
}

// ---- 以下为简单的 getter 方法 ----
qint64 FFmpegDecoder::GetDuration() const
{
    if (!fmt_ctx_) return 0;
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_ctx_);
    return fmt->duration != AV_NOPTS_VALUE ? fmt->duration * 1000 / AV_TIME_BASE : 0;
}

int FFmpegDecoder::GetWidth() const
{
    return codec_ctx_ ? codec_ctx_->width : 0;
}

int FFmpegDecoder::GetHeight() const
{
    return codec_ctx_ ? codec_ctx_->height : 0;
}

AVRational FFmpegDecoder::GetVideoTimeBase() const
{
    if (!fmt_ctx_ || video_stream_idx_ < 0) return { 0, 1 };
    return static_cast<AVFormatContext*>(fmt_ctx_)->streams[video_stream_idx_]->time_base;
}

AVFormatContext* FFmpegDecoder::GetFormatContext() const
{
    return fmt_ctx_;
}

AVCodecContext* FFmpegDecoder::GetCodecContext() const
{
    return codec_ctx_;
}

bool FFmpegDecoder::IsHardwareDecoding() const
{
    return is_hardware_;
}

// 读一个音频压缩包
// 从文件中一直读取，直到找到音频包或文件结束
AVPacket* FFmpegDecoder::ReadAudioPacket()
{
    if (!fmt_ctx_ || audio_stream_idx_ < 0) return nullptr;

    // 优先从队列取
    if (!audio_pkt_queue_.isEmpty())
    {
        return audio_pkt_queue_.dequeue();
    }

    // 队列空了，再从文件读
    while (true)
    {
        AVPacket* pkt = av_packet_alloc();
        int ret = av_read_frame(fmt_ctx_, pkt);

        if (ret < 0)
        {
            av_packet_free(&pkt);
            return nullptr;
        }

        if (pkt->stream_index == audio_stream_idx_)
        {
            return pkt;
        }

        if (pkt->stream_index == video_stream_idx_)
        {
            // 视频包直接投递给解码器，不缓存
            int send_ret = avcodec_send_packet(codec_ctx_, pkt);
            av_packet_free(&pkt);
            return nullptr;
        }

        av_packet_free(&pkt);  // 字幕等，丢掉
    }
}

// 获取视频帧率，用于解码线程做帧率控制
double FFmpegDecoder::GetFrameRate() const
{
    if (!fmt_ctx_ || video_stream_idx_ < 0) return 30.0;
    AVStream* stream = fmt_ctx_->streams[video_stream_idx_];
    // 优先使用 avg_frame_rate，然后 r_frame_rate，最后 codecpar 里的 framerate
    if (stream->avg_frame_rate.den > 0 && stream->avg_frame_rate.num > 0)
        return av_q2d(stream->avg_frame_rate);
    if (stream->r_frame_rate.den > 0 && stream->r_frame_rate.num > 0)
        return av_q2d(stream->r_frame_rate);
    if (stream->codecpar->framerate.den > 0 && stream->codecpar->framerate.num > 0)
        return av_q2d(stream->codecpar->framerate);
    return 30.0;  // 实在获取不到就默认 30fps
}

// 获取音频编码参数（给 AudioRenderer 用）
AVCodecParameters* FFmpegDecoder::GetAudioCodecPar() const
{
    return audio_codec_par_;
}

// 获取音频流索引
int FFmpegDecoder::GetAudioStreamIndex() const
{
    return audio_stream_idx_;
}

// 是否有音频流
bool FFmpegDecoder::HasAudioStream() const
{
    return audio_stream_idx_ >= 0;
}