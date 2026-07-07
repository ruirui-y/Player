#ifndef FFMPEGDECODER_H
#define FFMPEGDECODER_H

#include <QString>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVRational;

class FFmpegDecoder
{
public:
    FFmpegDecoder();
    ~FFmpegDecoder();

    bool OpenFile(const QString& path, bool try_hardware = false);
    void* GetD3D11Device() const;
    void Close();

    int  ReadFrame(AVFrame* frame);
    bool Seek(qint64 pos_ms);
    void FlushBuffers();

    qint64      GetDuration() const;
    int         GetWidth() const;
    int         GetHeight() const;
    AVRational  GetVideoTimeBase() const;
    AVFormatContext* GetFormatContext() const;
    AVCodecContext* GetCodecContext() const;
    bool        IsHardwareDecoding() const;
    double GetFrameRate() const;   // 获取视频帧率

private:
    AVFormatContext* fmt_ctx_{ nullptr };
    AVCodecContext* codec_ctx_{ nullptr };
    int              video_stream_idx_{ -1 };
    void* hw_device_ctx_{ nullptr };
    bool             is_hardware_{ false };
};

#endif // FFMPEGDECODER_H