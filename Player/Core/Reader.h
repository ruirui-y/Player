#ifndef READER_H
#define READER_H

#include <QString>
#include <atomic>
#include <thread>

struct AVFormatContext;
struct AVCodecParameters;
struct AVPacket;

template<typename T> class SafeQueue;

class Reader
{
public:
    explicit Reader(SafeQueue<AVPacket*>& video_packet_queue,
        SafeQueue<AVPacket*>& audio_packet_queue);                      // 构造，传入两个包队列

    ~Reader();                                                          // 析构

    bool Open(const QString& url);                                      // 打开文件或 RTSP 流
    void Start();                                                       // 启动读取线程
    void Stop();                                                        // 停止读取线程
    void Seek(qint64 pts_ms);                                           // 跳转到指定位置（毫秒）

    int  VideoStreamIndex() const;
    int  AudioStreamIndex() const;
    AVFormatContext* FormatContext() const;
    AVCodecParameters* AudioCodecParameters() const;

private:
    void ReadLoop();

    SafeQueue<AVPacket*>& video_packet_queue_;                          // 视频包输出队列
    SafeQueue<AVPacket*>& audio_packet_queue_;                          // 音频包输出队列

    AVFormatContext* fmt_ctx_{ nullptr };
    int  video_stream_idx_{ -1 };
    int  audio_stream_idx_{ -1 };
    AVCodecParameters* audio_codec_par_{ nullptr };

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<int64_t> seek_target_ms_{ -1 };
};

#endif // READER_H
