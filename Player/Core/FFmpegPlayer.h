#pragma once

#include <QObject>
#include <QImage>
#include <QTimer>
#include <atomic>
#include <thread>
#include "SafeQueue.h"
#include "Reader.h"
#include "Decoder.h"

class VideoRenderer;        // 视频渲染层（前向声明）
class AudioRenderer;        // 音频渲染层（前向声明）

struct AVPacket;
struct AVFrame;

class FFmpegPlayer : public QObject
{
    Q_OBJECT

public:
    explicit FFmpegPlayer(QObject* parent = nullptr);                                   // 构造
    ~FFmpegPlayer();                                                                    // 析构

    void SetVideoHwnd(HWND hwnd);                                                       // 设置渲染窗口句柄

    bool OpenFile(const QString& path);                                                 // 打开文件
    void Play();                                                                        // 开始/恢复播放
    void Pause();                                                                       // 暂停
    void Stop();                                                                        // 停止
    void Close();                                                                       // 关闭全部资源

    qint64 GetPosition() const;                                                         // 当前播放位置（毫秒）
    qint64 GetDuration() const;                                                         // 总时长（毫秒）
    bool   IsPlaying() const;                                                           // 是否正在播放
    bool   IsPaused() const;                                                            // 是否暂停

signals:
    void SigLoaded(qint64 duration_ms);                                                 // 文件加载完成
    void SigFrameReady(const QImage& image);                                            // 软解一帧就绪（回退路径）
    void SigFinished();                                                                 // 播放到结尾
    void SigError(const QString& msg);                                                  // 发生错误
    void SigPlayState(const QString& state);                                            // 播放状态变化

private:
    void DecodeLoop();                                                                  // 解码线程主循环

    VideoRenderer* video_renderer_{ nullptr };                                          // 视频渲染层
    AudioRenderer* audio_renderer_{ nullptr };                                          // 音频渲染层

    HWND hwnd_{ nullptr };                                                              // 窗口句柄

    std::thread         decode_thread_;                                                 // 解码线程
    std::atomic<bool>   playing_{ false };                                              // 播放中
    std::atomic<bool>   paused_{ false };                                               // 已暂停
    std::atomic<qint64> current_pts_ms_{ 0 };                                           // 当前播放位置（毫秒）
    qint64              duration_ms_{ 0 };                                              // 视频总时长

    // 队列
    SafeQueue<AVPacket*> packet_queue_;
    SafeQueue<AVFrame*>  video_queue_;
    SafeQueue<AVFrame*>  audio_queue_;

    Reader  reader_{ packet_queue_ };
    Decoder decoder_{ packet_queue_, video_queue_, audio_queue_ };
};