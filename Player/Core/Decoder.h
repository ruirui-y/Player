// Decoder.h
#ifndef DECODER_H
#define DECODER_H

#include <atomic>
#include <thread>

struct AVCodecContext;
struct AVCodecParameters;
struct AVBufferRef;
struct AVPacket;
struct AVFrame;
struct ID3D11Device;

template<typename T> class SafeQueue;

class Decoder
{
public:
    Decoder(SafeQueue<AVPacket*>& packet_queue,
        SafeQueue<AVFrame*>& video_queue,
        SafeQueue<AVFrame*>& audio_queue);                          // 构造
    ~Decoder();                                                     // 析构

    void SetStreamIndex(int video_idx, int audio_idx);             // 告诉decoder音视频包的stream_index

    bool OpenVideo(AVCodecParameters* video_par,
        bool try_hardware = false);                                 // 打开视频解码器（支持硬解）
    bool OpenAudio(AVCodecParameters* audio_par);                   // 打开音频解码器
    void Start();                                                   // 启动解码线程
    void Stop();                                                    // 停止解码线程
    void Flush();                                                   // 刷新解码器（seek 后调用）

    AVCodecContext* VideoCodecContext() const;                      // 给渲染器取 pix_fmt 等
    AVCodecContext* AudioCodecContext() const;                      // 给音频渲染器取参数
    bool IsHardwareDecoding() const;                                // 是否正在使用硬解
    ID3D11Device* GetD3D11Device() const;                           // 获取硬件设备（给渲染器创建交换链）

private:
    void DecodeLoop();                                              // 解码线程主循环
    void DecodeVideoPacket(AVPacket* pkt);                          // 解码一个视频包
    void DecodeAudioPacket(AVPacket* pkt);                          // 解码一个音频包

    SafeQueue<AVPacket*>& packet_queue_;                            // 输入的包队列
    SafeQueue<AVFrame*>& video_queue_;                              // 输出的视频帧队列
    SafeQueue<AVFrame*>& audio_queue_;                              // 输出的音频帧队列

    AVCodecContext* video_ctx_{ nullptr };                          // 视频解码器上下文
    AVCodecContext* audio_ctx_{ nullptr };                          // 音频解码器上下文
    AVBufferRef* hw_device_ctx_{ nullptr };                         // 硬解设备上下文
    bool            is_hardware_{ false };                          // 硬解是否成功初始化
    int             video_stream_index_{ 0 };                       // 视频流索引（Reader 传入）
    int             audio_stream_index_{ 1 };                       // 音频流索引（Reader 传入）

    std::thread thread_;                                            // 解码线程
    std::atomic<bool> running_{ false };                            // 运行标志
};

#endif // DECODER_H