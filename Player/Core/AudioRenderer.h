#ifndef AUDIORENDERER_H
#define AUDIORENDERER_H

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QQueue>

#include <QAudioOutput>
#include <atomic>

struct AVCodecContext;
struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
struct SwrContext;
class QIODevice;

// 音频渲染器
// 职责：解码音频包 → 重采样为 PCM → 喂给 QAudioSink 播放
class AudioRenderer : public QObject
{
    Q_OBJECT

public:
    explicit AudioRenderer(QObject* parent = nullptr);
    ~AudioRenderer();

    bool Open(AVCodecParameters* codecpar);                                     // 打开音频解码器
    void Start();                                                               // 开始播放
    void Pause();                                                               // 暂停
    void Resume();                                                              // 恢复
    void Stop();                                                                // 停止
    void Close();                                                               // 关闭释放
    void Flush();                                                               // 清空缓存

    bool DecodePacket(AVPacket* packet);                                        // 解码一个音频包（解码线程调用）
    double GetClock() const;                                                    // 当前音频时钟（毫秒）

    bool IsOpened() const { return codec_ctx_ != nullptr; }
    bool IsPlaying() const { return playing_; }
    bool IsPaused() const { return paused_; }

signals:
    void SigFinished();                                                         // 音频播放完毕

private:
    void FeedPcmData(const QByteArray& pcm_data);                               // 把 PCM 数据写入音频设备

    AVCodecContext* codec_ctx_{ nullptr };                                      // 音频解码器上下文
    SwrContext* swr_ctx_{ nullptr };                                            // 重采样上下文

    QAudioOutput* audio_sink_{ nullptr };                                       // Qt 音频输出
    QIODevice* audio_io_{ nullptr };                                            // 音频 I/O 通道

    QQueue<QByteArray> pcm_queue_;                                              // PCM 数据队列
    mutable QMutex    queue_mutex_;                                             // 队列线程锁

    std::atomic<bool> playing_{ false };                                        // 正在播放
    std::atomic<bool> paused_{ false };                                         // 已暂停

    double          audio_clock_{ 0.0 };                                        // 音频时钟（毫秒）
    int             sample_rate_{ 48000 };                                      // 重采样目标采样率
    qint64          total_written_{ 0 };                                        // 已写入音频设备的总字节数
};

#endif // AUDIORENDERER_H