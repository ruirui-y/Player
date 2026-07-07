#ifndef FFMPEGCORE_H
#define FFMPEGCORE_H

#include <QObject>
#include <QImage>
#include <QTimer>
#include <atomic>
#include <thread>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class FFmpegCore : public QObject
{
    Q_OBJECT

public:
    explicit FFmpegCore(QObject* parent = nullptr);
    ~FFmpegCore();

    bool OpenFile(const QString& path);                                 // 打开文件，只加载不播放
    void Play();                                                        // 开始/恢复播放
    void Pause();                                                       // 暂停
    void Stop();                                                        // 停止播放（不清除文件）
    void Close();                                                       // 关闭文件并释放全部资源
    qint64 GetPosition() const;                                         // 当前播放位置(毫秒)
    qint64 GetDuration() const;                                         // 总时长(毫秒)
    bool IsPlaying() const;                                             // 是否正在播放
    bool IsPaused() const;                                              // 是否暂停

signals:
    void SigLoaded(qint64 duration_ms);                                 // 文件加载完成，可以播放
    void SigFrameReady(const QImage& image);                            // 一帧解码完成（跨线程发送到主线程）
    void SigFinished();                                                 // 播放到结尾
    void SigError(const QString& msg);                                  // 发生错误
    void SigPlayState(const QString& state);                            // 播放状态变化

private:
    void DecodeThreadFunc();                                            // 解码线程主循环
    void RenderFrame(AVFrame* frame);                                   // 用 sws_scale 将 AVFrame 转 QImage 并发射

    // ---- FFmpeg 相关 ----
    AVFormatContext* fmt_ctx_{ nullptr };                               // 解封装上下文
    AVCodecContext* codec_ctx_{ nullptr };                              // 视频解码器上下文
    int              video_stream_idx_{ -1 };                           // 视频流索引
    SwsContext* sws_ctx_{ nullptr };                                    // 图像格式转换上下文

    // ---- 播放状态 ----
    std::atomic<bool>        playing_{ false };                         // 解码线程是否在运行
    std::atomic<bool>        paused_{ false };                          // 是否暂停
    std::atomic<qint64>      current_pts_ms_{ 0 };                      // 当前播放位置(毫秒)，解码线程更新
    qint64                   duration_ms_{ 0 };                         // 文件总时长(毫秒)
    int                      frame_width_{ 0 };                         // 视频宽度
    int                      frame_height_{ 0 };                        // 视频高度

    // ---- 线程 ----
    std::thread              decode_thread_;                            // 解码线程
};

#endif // FFMPEGCORE_H