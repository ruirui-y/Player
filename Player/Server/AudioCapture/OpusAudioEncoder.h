#ifndef OPUSAUDIOENCODER_H
#define OPUSAUDIOENCODER_H

#include <cstdint>
#include <vector>

#include <opus/opus.h>

// Opus 音频编码器
// 职责：将 48kHz/2ch/16-bit PCM 编码为 Opus 压缩数据
// 每次 Encode() 处理一帧（默认 20ms = 960 samples/channel）
class OpusAudioEncoder
{
public:
    OpusAudioEncoder();                                             // 构造
    ~OpusAudioEncoder();                                            // 析构

    // 初始化编码器
    // sample_rate：采样率（如 48000）
    // channels：声道数（1 或 2）
    // bitrate_bps：码率（如 128000 = 128kbps），0 = 自动
    // frame_ms：帧时长（如 20ms）
    bool Init(uint32_t sample_rate = 48000, uint32_t channels = 2,
              uint32_t bitrate_bps = 128000, uint32_t frame_ms = 20);   // 初始化

    // 编码一帧 PCM 数据
    // pcm_data：交错排列的 16-bit PCM
    // sample_count_per_channel：每声道采样数（如 960 = 20ms@48kHz）
    // output：编码后的 Opus 数据
    // 返回：编码后的字节数，<0 表示错误
    int Encode(const int16_t* pcm_data, int sample_count_per_channel,
               std::vector<uint8_t>& output);                       // 编码一帧

    int FrameSamples() const { return frame_samples_; }            // 每帧的采样数（每声道）
    uint32_t FrameMs() const { return frame_ms_; }                 // 每帧的时长（毫秒）

private:
    OpusEncoder* encoder_{nullptr};                                 // Opus 编码器实例
    uint32_t sample_rate_{48000};                                   // 采样率
    uint32_t channels_{2};                                          // 声道数
    uint32_t frame_ms_{20};                                         // 帧时长（毫秒）
    int frame_samples_{960};                                        // 每帧采样数（每声道）

    std::vector<uint8_t> output_buffer_;                            // 编码输出缓冲区
};

#endif // OPUSAUDIOENCODER_H
