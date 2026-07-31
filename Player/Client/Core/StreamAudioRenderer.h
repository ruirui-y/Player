#ifndef STREAMAUDIORENDERER_H
#define STREAMAUDIORENDERER_H

#include <QObject>
#include <QByteArray>
#include <atomic>
#include <cstdint>

struct AVFrame;
struct SwrContext;
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;

// 串流音频渲染器（WASAPI 共享模式，封装 IAudioRenderClient）
// 职责：接收 Opus 解码后的 float32-planar 帧 → swr 重采样为 S16 PCM → WASAPI 播放
// 专用于串流模式，绕过 Qt 的 QAudioOutput（避免 Release 下设备枚举失败）
class StreamAudioRenderer : public QObject
{
    Q_OBJECT

public:
    explicit StreamAudioRenderer(QObject* parent = nullptr);                    // 构造
    ~StreamAudioRenderer();                                                     // 析构

    void Start();                                                               // 打开 WASAPI 设备 + 创建 swr
    void Stop();                                                                // 停止 WASAPI
    void Close();                                                               // 关闭释放

    bool CanAcceptFrame() const;                                                // WASAPI 缓冲区能否接受一帧音频数据？
    bool FeedFrame(AVFrame* frame);                                             // 接收已解码的音频帧（Decoder 线程调用）

    void SetVolume(double volume);                                              // 0.0 ~ 1.0（软件音量）
    double GetVolume() const;
    bool IsOpened() const { return render_client_ != nullptr; }

private:
    // WASAPI COM 对象
    IMMDeviceEnumerator* enumerator_{ nullptr };                                // 设备枚举器
    IMMDevice* device_{ nullptr };                                              // 默认渲染设备
    IAudioClient* audio_client_{ nullptr };                                     // 音频客户端
    IAudioRenderClient* render_client_{ nullptr };                              // 渲染客户端

    SwrContext* swr_ctx_{ nullptr };                                            // 重采样上下文（float32-planar → S16）

    uint32_t sample_rate_{ 48000 };                                             // 采样率
    uint32_t channels_{ 2 };                                                    // 声道数
    uint32_t buffer_frames_{ 0 };                                               // 硬件缓冲区帧数
    int frame_bytes_{ 0 };                                                      // 缓存一帧 PCM 大小（字节）
    double volume_{ 1.0 };                                                      // 音量
};

#endif // STREAMAUDIORENDERER_H