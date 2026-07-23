#ifndef AUDIODECODER_H
#define AUDIODECODER_H

#include <atomic>
#include <thread>

struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;

template<typename T> class SafeQueue;

class AudioDecoder
{
public:
    AudioDecoder(SafeQueue<AVPacket*>& packet_queue,
        SafeQueue<AVFrame*>& frame_queue);                                              // 构造
    ~AudioDecoder();                                                                    // 析构

    bool OpenAudio(AVCodecParameters* audio_par);                                       // 打开音频解码器
    void Start();                                                                       // 启动解码线程
    void Stop();                                                                        // 停止解码线程
    void Flush();                                                                       // 刷新解码器缓存

    AVCodecContext* AudioCodecContext() const;                                          // 给 AudioRenderer 取参数
    void SetStreamIndex(int index);                                                     // 设置音频流索引

private:
    void AudioDecodeLoop();                                                             // 解码线程主循环
    void DecodePacket(AVPacket* pkt);                                                   // 解码单个包

    SafeQueue<AVPacket*>& packet_queue_;                                                // 输入的压缩包队列
    SafeQueue<AVFrame*>& frame_queue_;                                                  // 输出的解码帧队列

    AVCodecContext* audio_ctx_{ nullptr };                                              // 音频解码器上下文
    int             stream_index_{ 1 };                                                 // 音频流索引

    std::thread         thread_;                                                        // 解码线程
    std::atomic<bool>   running_{ false };                                              // 运行标志
};

#endif // AUDIODECODER_H