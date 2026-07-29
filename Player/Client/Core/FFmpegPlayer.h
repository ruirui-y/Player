#pragma once

#include <QObject>
#include <QImage>
#include <QTimer>
#include <atomic>
#include <thread>
#include "Common/SafeQueue.h"
#include "Reader.h"
#include "VideoDecoder.h"
#include "AudioDecoder.h"
#include "Common/ClockUtil.h"

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
    void Seek(qint64 pts_ms);                                                           // 跳转到指定位置（毫秒）
    void Stop();                                                                        // 停止
    void Close();                                                                       // 关闭全部资源

    qint64 GetPosition() const;                                                         // 当前播放位置（毫秒）
    qint64 GetDuration() const;                                                         // 总时长（毫秒）
    bool   IsPlaying() const;                                                           // 是否正在播放
    bool   IsPaused() const;                                                            // 是否暂停

    void SetVolume(double volume);                                                      // 0.0 ~ 1.0
    double GetVolume() const;

signals:
    void SigLoaded(qint64 duration_ms);                                                 // 文件加载完成
    void SigFrameReady(const QImage& image);                                            // 软解一帧就绪（回退路径）
    void SigFinished();                                                                 // 播放到结尾
    void SigError(const QString& msg);                                                  // 发生错误
    void SigPlayState(const QString& state);                                            // 播放状态变化

private:
    void DecodeLoop();                                                                  // 渲染线程主循环

    VideoRenderer* video_renderer_{ nullptr };                                          // 视频渲染层
    AudioRenderer* audio_renderer_{ nullptr };                                          // 音频渲染层

    HWND hwnd_{ nullptr };                                                              // 窗口句柄

    std::thread         decode_thread_;                                                 // 渲染线程
    std::atomic<bool>   playing_{ false };                                              // 播放中
    std::atomic<bool>   paused_{ false };                                               // 已暂停
    std::atomic<qint64> current_pts_ms_{ 0 };                                           // 当前播放位置（毫秒）
    qint64              duration_ms_{ 0 };                                              // 视频总时长

    // ---- 音视频同步相关 ----
    double frame_timer_ms_{ 0.0 };                                                      // frame_timer：上一帧应该显示的时间点（毫秒）
    double video_clock_ms_{ 0.0 };                                                      // vidclk：当前视频帧 PTS（毫秒）
    double last_frame_pts_ms_{ 0.0 };                                                   // 上一帧 PTS（毫秒）
    double pause_start_time_ms_{ 0.0 };                                                 // 暂停时的系统时间（毫秒），恢复时补偿 frame_timer
    int    frame_drops_early_{ 0 };                                                     // 早期丢帧计数
    int    frame_drops_late_{ 0 };                                                      // 晚期丢帧计数

    // ---- 队列体系 ----
    SafeQueue<AVPacket*> video_packet_queue_;                                           // 视频压缩包队列
    SafeQueue<AVPacket*> audio_packet_queue_;                                           // 音频压缩包队列
    SafeQueue<AVFrame*>  video_frame_queue_;                                            // 视频帧队列
    SafeQueue<AVFrame*>  audio_frame_queue_;                                            // 音频帧队列

    // ---- 核心对象 ----
    Reader        reader_{ video_packet_queue_, audio_packet_queue_ };
    VideoDecoder  video_decoder_{ video_packet_queue_, video_frame_queue_ };
    AudioDecoder  audio_decoder_{ audio_packet_queue_, audio_frame_queue_ };
};
