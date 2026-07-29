#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <atomic>
#include <thread>

struct AVCodecContext;
struct AVCodecParameters;
struct AVBufferRef;
struct AVPacket;
struct AVFrame;
struct ID3D11Device;

template<typename T> class SafeQueue;

class VideoDecoder
{
public:
    VideoDecoder(SafeQueue<AVPacket*>& packet_queue,
        SafeQueue<AVFrame*>& frame_queue);                                          // 构造
    ~VideoDecoder();                                                                // 析构

    bool OpenVideo(AVCodecParameters* video_par,
        bool try_hardware = false);                                                 // 打开视频解码器（支持硬解）
    void Start();                                                                   // 启动解码线程
    void Stop();                                                                    // 停止解码线程
    void Flush();                                                                   // 刷新解码器缓存

    AVCodecContext* VideoCodecContext() const;                                      // 给渲染器取 pix_fmt 等
    bool IsHardwareDecoding() const;                                                // 是否正在使用硬解
    ID3D11Device* GetD3D11Device() const;                                           // 获取硬件设备
    void SetStreamIndex(int index);                                                 // 设置视频流索引

private:
    void VideoDecodeLoop();                                                         // 解码线程主循环
    void DecodePacket(AVPacket* pkt);                                               // 解码单个包

    SafeQueue<AVPacket*>& packet_queue_;                                            // 输入的压缩包队列
    SafeQueue<AVFrame*>& frame_queue_;                                              // 输出的解码帧队列

    AVCodecContext* video_ctx_{ nullptr };                                          // 视频解码器上下文
    AVBufferRef* hw_device_ctx_{ nullptr };                                         // 硬解设备上下文
    bool            is_hardware_{ false };                                          // 硬解是否成功
    int             stream_index_{ 0 };                                             // 视频流索引

    std::thread         thread_;                                                    // 解码线程
    std::atomic<bool>   running_{ false };                                          // 运行标志
    int last_serial_{ -1 };                                                         // 解码器记录serial, 变化时flush
};

#endif // VIDEODECODER_H
