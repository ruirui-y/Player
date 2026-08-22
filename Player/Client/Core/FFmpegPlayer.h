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
#include "Common/NetWork/VideoReceiver.h"
#include "Common/NetWork/AudioReceiver.h"

class VideoRenderer;        // 视频渲染层（前向声明）
class AudioRenderer;        // 音频渲染层（前向声明，文件播放专用）
class StreamAudioRenderer;  // 串流音频渲染层（前向声明，串流专用）
class InputTransportClient; // 控制信道客户端（前向声明，IDR 请求用）
class RttMeasurer;          // RTT 测量器（前向声明）
class Pacer;                // VSync 帧步调器（前向声明）

struct AVPacket;
struct AVFrame;
struct AVRational;
struct AVCodecParameters;

class FFmpegPlayer : public QObject
{
    Q_OBJECT

public:
    explicit FFmpegPlayer(QObject* parent = nullptr);                                                       // 构造
    ~FFmpegPlayer();                                                                                        // 析构

    void SetVideoHwnd(HWND hwnd);                                                                           // 设置渲染窗口句柄

    bool OpenFile(const QString& path);                                                                     // 打开文件
    bool OpenStream(uint16_t video_port, int fps, uint16_t audio_port = 47997);                             // 打开网络串流（低延迟模式，分辨率从码流自动获取）
    void Play();                                                                                            // 开始/恢复播放
    void Pause();                                                                                           // 暂停
    void Seek(qint64 pts_ms);                                                                               // 跳转到指定位置（毫秒）
    void Stop();                                                                                            // 停止
    void Close();                                                                                           // 关闭全部资源

    qint64 GetPosition() const;                                                                             // 当前播放位置（毫秒）
    qint64 GetDuration() const;                                                                             // 总时长（毫秒）
    bool   IsPlaying() const;                                                                               // 是否正在播放
    bool   IsPaused() const;                                                                                // 是否暂停

    // 获取视频流发送方 IP（串流模式，收到第一个 UDP 包后才有值）
    std::string GetSenderIP() const;                                                                        // 获取发送方 IP

    // 设置控制信道绑定（串流模式）
    // 绑定 IDR 请求 + RTT 测量 + 网络统计上报
    void SetupStreamControl(InputTransportClient* input_transport);                                         // 绑定控制信道

    // 串流模式网络统计（供 StreamWindow OSD 读取）
    int   GetReceiveFps() const;                                                                            // VideoReceiver 接收 FPS
    int   GetRenderFps() const;                                                                             // Pacer 渲染 FPS
    int   GetRttMs() const;                                                                                 // RttMeasurer 最新 RTT

    void SetVolume(double volume);                                                                          // 0.0 ~ 1.0
    double GetVolume() const;

signals:
    void SigLoaded(qint64 duration_ms);                                                                     // 文件加载完成
    void SigFrameReady(const QImage& image);                                                                // 软解一帧就绪（回退路径）
    void SigFinished();                                                                                     // 播放到结尾
    void SigError(const QString& msg);                                                                      // 发生错误
    void SigPlayState(const QString& state);                                                                // 播放状态变化
    void SigSenderIPReady(const QString& ip);                                                               // 首包到达，已知服务端 IP

private:
    void DecodeLoop();                                                                                      // 渲染线程主循环

    // ---- OpenStream 子阶段（从 OpenStream 拆分，便于独立测试）----
    bool InitVideoDecoder();                                                                                // 构造 H.264 AVCodecParameters 并打开视频解码器（硬解失败回退软解）
    bool InitAudioDecoder();                                                                                // 构造 Opus AVCodecParameters 并打开音频解码器（失败仅 WARN）
    bool InitVideoReceiver(uint16_t video_port);                                                            // 创建并初始化 UDP 视频接收器 + 绑定服务端 IP 回调
    bool InitAudioReceiver(uint16_t audio_port);                                                            // 创建并初始化 UDP 音频接收器（失败仅 WARN）
    void InitPacer();                                                                                       // 创建 VSync 帧步调器（按需）

    void GetStreamInfo(double& fps, AVRational& video_tb, bool& has_audio, double& frame_interval_ms);      // 获取流信息（帧率、时基、音频标志）
    bool WaitForFrameQueues(bool has_audio);                                                                // 等待帧队列就绪，返回 false 表示退出
    void InitSyncState();                                                                                   // 初始化同步计数器
    void ProcessStreamingLoop(bool has_audio, AVRational video_tb);                                         // 串流主循环（喂音频 + 渲染视频）
    void ProcessFileLoop(bool has_audio, AVRational video_tb, double fps, double frame_interval_ms);        // 文件播放主循环（音视频同步 + 渲染）
    void CleanupFrames();                                                                                   // 清理残留帧队列

    VideoRenderer* video_renderer_{ nullptr };                                                              // 视频渲染层
    AudioRenderer* audio_renderer_{ nullptr };                                                              // 音频渲染层（文件播放）
    StreamAudioRenderer* stream_audio_renderer_{ nullptr };                                                 // 音频渲染层（串流播放）

    HWND hwnd_{ nullptr };                                                                                  // 窗口句柄

    std::thread         decode_thread_;                                                                     // 渲染线程
    std::atomic<bool>   playing_{ false };                                                                  // 播放中
    std::atomic<bool>   paused_{ false };                                                                   // 已暂停
    std::atomic<qint64> current_pts_ms_{ 0 };                                                               // 当前播放位置（毫秒）
    qint64              duration_ms_{ 0 };                                                                  // 视频总时长

    // ---- 音视频同步相关 ----
    double frame_timer_ms_{ 0.0 };                                                                          // frame_timer：上一帧应该显示的时间点（毫秒）
    double video_clock_ms_{ 0.0 };                                                                          // vidclk：当前视频帧 PTS（毫秒）
    double last_frame_pts_ms_{ 0.0 };                                                                       // 上一帧 PTS（毫秒）
    double pause_start_time_ms_{ 0.0 };                                                                     // 暂停时的系统时间（毫秒），恢复时补偿 frame_timer
    int    frame_drops_early_{ 0 };                                                                         // 早期丢帧计数
    int    frame_drops_late_{ 0 };                                                                          // 晚期丢帧计数

    // ---- 队列体系 ----
    SafeQueue<AVPacket*> video_packet_queue_;                                                               // 视频压缩包队列
    SafeQueue<AVPacket*> audio_packet_queue_;                                                               // 音频压缩包队列
    SafeQueue<AVFrame*>  video_frame_queue_;                                                                // 视频帧队列
    SafeQueue<AVFrame*>  audio_frame_queue_;                                                                // 音频帧队列

    // ---- 核心对象 ----
    Reader        reader_{ video_packet_queue_, audio_packet_queue_ };
    VideoDecoder  video_decoder_{ video_packet_queue_, video_frame_queue_ };
    AudioDecoder  audio_decoder_{ audio_packet_queue_, audio_frame_queue_ };

    // ---- 串流模式 ----
    VideoReceiver* video_receiver_{ nullptr };                                                              // UDP 视频接收器（串流模式使用）
    AudioReceiver* audio_receiver_{ nullptr };                                                              // UDP 音频接收器（串流模式使用）
    RttMeasurer*   rtt_measurer_{ nullptr };                                                                // RTT 测量器（串流模式使用）
    Pacer*         pacer_{ nullptr };                                                                       // VSync 帧步调器
    bool is_streaming_{ false };                                                                            // 是否串流模式
    int  stream_fps_{ 60 };                                                                                 // 串流帧率
    bool renderer_inited_{ false };                                                                         // 渲染器是否已初始化（延迟到首帧）
    bool closed_{ true };                                                                                   // 是否已关闭（独立标志，替代 duration_ms_ 判断）
};