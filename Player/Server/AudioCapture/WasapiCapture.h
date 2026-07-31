#ifndef WASAPICAPTURE_H
#define WASAPICAPTURE_H

#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>

#include <mmdeviceapi.h>
#include <audioclient.h>

struct SwrContext;

// WASAPI 环回采集器
// 职责：采集系统默认播放设备的音频（loopback）
// 内部使用 SwrContext 完成 float32→int16 格式转换 + 采样率重采样
// 无论设备原始格式如何，始终输出 48kHz/2ch/16-bit PCM
class WasapiCapture
{
public:
    // PCM 数据回调
    // pcm_data：交错排列的 16-bit PCM 数据（已重采样到 output_sample_rate_）
    // sample_count：每声道的采样数
    // timestamp_ms：时间戳（毫秒）
    using AudioCallback = std::function<void(const int16_t* pcm_data, int sample_count, uint32_t timestamp_ms)>;

    WasapiCapture();                                                // 构造
    ~WasapiCapture();                                               // 析构

    // 初始化 WASAPI 环回采集
    // output_sample_rate：输出采样率（如 48000，Opus 只支持 8/12/16/24/48kHz）
    // channels：声道数（如 2）
    bool Init(uint32_t output_sample_rate = 48000, uint32_t channels = 2);  // 初始化

    void SetCallback(AudioCallback cb);                             // 设置音频数据回调

    void Start();                                                   // 启动采集线程
    void Stop();                                                    // 停止采集线程

    bool IsRunning() const { return running_; }                     // 是否运行中
    uint32_t SampleRate() const { return output_sample_rate_; }     // 输出采样率（重采样后）
    uint32_t Channels() const { return channels_; }                 // 声道数

private:
    void CaptureLoop();                                             // 采集线程主循环

    // ---- WASAPI 接口 ----
    IMMDeviceEnumerator* enumerator_{nullptr};                     // 设备枚举器
    IMMDevice* device_{nullptr};                                   // 默认音频设备
    IAudioClient* audio_client_{nullptr};                          // 音频客户端（采集）
    IAudioCaptureClient* capture_client_{nullptr};                 // 采集客户端
    IAudioClient* render_client_{nullptr};                         // 辅助渲染客户端（保持音频引擎活跃）

    // ---- 重采样 ----
    SwrContext* swr_ctx_{nullptr};                                 // FFmpeg 重采样上下文
    uint32_t device_sample_rate_{48000};                           // 设备原始采样率
    uint32_t output_sample_rate_{48000};                           // 输出采样率（重采样目标）
    uint32_t channels_{2};                                         // 声道数
    WAVEFORMATEX* pwfex_{nullptr};                                 // WASAPI 返回的音频格式

    // ---- 线程 ----
    std::thread capture_thread_;                                   // 采集线程
    std::atomic<bool> running_{false};                             // 运行标志

    // ---- 回调 ----
    AudioCallback callback_;                                        // 音频数据回调

    // ---- COM ----
    bool com_initialized_{false};                                  // 是否成功初始化 COM
};

#endif // WASAPICAPTURE_H
