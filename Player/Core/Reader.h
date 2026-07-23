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
    explicit Reader(SafeQueue<AVPacket*>& packet_queue);                                // 构造，传入包队列
    ~Reader();                                                                          // 析构

    bool Open(const QString& url);                                                      // 打开文件或 RTSP 流
    void Start();                                                                       // 启动读取线程
    void Stop();                                                                        // 停止读取线程
    void Seek(qint64 pts_ms);                                                           // 跳转到指定位置（毫秒）

    // 流信息获取（Open 之后调用）
    int  VideoStreamIndex() const;
    int  AudioStreamIndex() const;
    AVFormatContext* FormatContext() const;
    AVCodecParameters* AudioCodecParameters() const;

private:
    void ReadLoop();                                                                    // 读取线程主循环

    SafeQueue<AVPacket*>& packet_queue_;                                                // 输出的包队列

    AVFormatContext* fmt_ctx_{ nullptr };                                               // FFmpeg 解封装上下文
    int  video_stream_idx_{ -1 };                                                       // 视频流索引
    int  audio_stream_idx_{ -1 };                                                       // 音频流索引
    AVCodecParameters* audio_codec_par_{ nullptr };                                     // 音频编码参数（给 Decoder 用）

    std::thread thread_;                                                                // 读取线程
    std::atomic<bool> running_{ false };                                                // 运行标志
    std::atomic<int64_t> seek_target_ms_{ -1 };                                         // 跳转目标（-1 = 无跳转）
};

#endif // READER_H