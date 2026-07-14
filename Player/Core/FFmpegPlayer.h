#pragma once

#include <QObject>
#include <QImage>
#include <QTimer>
#include <atomic>
#include <thread>

class FFmpegDecoder;        // 解码层（前向声明）
class D3D11Pipeline;        // GPU 渲染管线（前向声明）
class Nv12GpuUploader;      // GPU 帧上传器（前向声明）
class SoftwareRenderer;     // CPU 软解渲染器（前向声明）

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

    FFmpegDecoder*      decoder_{ nullptr };                                            // 解码层
    D3D11Pipeline*      d3d11_pipeline_{ nullptr };                                     // GPU 渲染管线
    Nv12GpuUploader*    nv12_uploader_{ nullptr };                                      // GPU 帧上传器
    SoftwareRenderer*   sw_renderer_{ nullptr };                                        // CPU 软解渲染器

    HWND hwnd_{ nullptr };                                                              // 窗口句柄

    std::thread         decode_thread_;                                                 // 解码线程
    std::atomic<bool>   playing_{ false };                                              // 播放中
    std::atomic<bool>   paused_{ false };                                               // 已暂停
    std::atomic<qint64> current_pts_ms_{ 0 };                                           // 当前播放位置（毫秒）
    qint64              duration_ms_{ 0 };                                              // 视频总时长
};
