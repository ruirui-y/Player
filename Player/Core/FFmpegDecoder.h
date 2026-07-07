#ifndef FFMPEGDECODER_H
#define FFMPEGDECODER_H

#include <QString>

struct AVFormatContext;     // FFmpeg 解封装上下文（前向声明）
struct AVCodecContext;      // FFmpeg 解码器上下文（前向声明）
struct AVFrame;             // FFmpeg 帧结构（前向声明）
struct AVRational;          // FFmpeg 有理数结构（前向声明）

class FFmpegDecoder
{
public:
    FFmpegDecoder();
    ~FFmpegDecoder();

    bool OpenFile(const QString& path,
        bool try_hardware = false);                                                 // 打开文件，true=尝试硬解
    void* GetD3D11Device() const;                                                   // 返回解码器内部的 ID3D11Device*
    void Close();                                                                   // 关闭文件，释放所有 FFmpeg 资源

    int  ReadFrame(AVFrame* frame);                                                 // 读一包 → 解一帧，0=有帧 <0=结束
    bool Seek(qint64 pos_ms);                                                       // 跳转到指定毫秒
    void FlushBuffers();                                                            // 清空解码器缓存（seek 后必须调用）

    qint64      GetDuration() const;                                                // 视频总时长（毫秒）
    int         GetWidth() const;                                                   // 视频宽度
    int         GetHeight() const;                                                  // 视频高度
    AVRational  GetVideoTimeBase() const;                                           // 视频流 time_base，PTS 计算用
    AVFormatContext* GetFormatContext() const;                                      // 给播放线程取流信息用
    AVCodecContext* GetCodecContext() const;                                        // 给渲染线程取 pix_fmt 用
    bool        IsHardwareDecoding() const;                                         // 当前是否正在使用硬解
    double      GetFrameRate() const;                                               // 获取视频帧率

private:
    AVFormatContext* fmt_ctx_{ nullptr };                                           // 解封装上下文
    AVCodecContext* codec_ctx_{ nullptr };                                          // 解码器上下文
    int              video_stream_idx_{ -1 };                                       // 视频流在文件中的索引
    void* hw_device_ctx_{ nullptr };                                                // AVBufferRef*，硬解设备上下文
    bool             is_hardware_{ false };                                         // 硬解是否成功初始化
};

#endif // FFMPEGDECODER_H
