#include "OpusAudioEncoder.h"

#include "Common/LogManager.h"

// 构造
OpusAudioEncoder::OpusAudioEncoder()
{
}

// 析构
OpusAudioEncoder::~OpusAudioEncoder()
{
    if (encoder_)
    {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
}

// ================================================================
// ---- 初始化：创建编码器 → 设置码率/复杂度 → 预分配缓冲区 ----
// ================================================================
bool OpusAudioEncoder::Init(uint32_t sample_rate, uint32_t channels,
                            uint32_t bitrate_bps, uint32_t frame_ms)
{
    sample_rate_ = sample_rate;
    channels_ = channels;
    frame_ms_ = frame_ms;
    frame_samples_ = static_cast<int>(sample_rate * frame_ms / 1000);

    // ---- 第一步：创建 Opus 编码器 ----
    int error = 0;
    encoder_ = opus_encoder_create(sample_rate, channels,
                                   OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !encoder_)
    {
        LogManager::Log("ERR", "[OpusEncoder] opus_encoder_create 失败, error = %d", error);
        return false;
    }

    // ---- 第二步：设置编码参数 ----
    if (bitrate_bps > 0)
    {
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate_bps));
    }

    // 最高复杂度（质量优先，CPU 开销可忽略）
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(10));

    // 禁用静音压缩（DTX），保证连续音频流
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));

    // 禁用带内 FEC（减少额外开销，丢包由 UDP 丢帧处理）
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(0));

    // 预分配输出缓冲区（Opus 最大编码输出 = 每声道 4 字节 × 采样数）
    output_buffer_.resize(frame_samples_ * channels_ * 4);

    LogManager::Log("INFO", "[OpusEncoder] 初始化成功: %dHz %dch %dms %dbps",
                    sample_rate, channels, frame_ms, bitrate_bps);
    return true;
}

// 编码一帧 PCM 数据
int OpusAudioEncoder::Encode(const int16_t* pcm_data, int sample_count_per_channel,
                             std::vector<uint8_t>& output)
{
    if (!encoder_ || !pcm_data)
        return -1;

    // opus_encode 返回编码后的字节数
    opus_int32 encoded_bytes = opus_encode(encoder_, pcm_data,
                                           sample_count_per_channel,
                                           output_buffer_.data(),
                                           static_cast<opus_int32>(output_buffer_.size()));

    if (encoded_bytes < 0)
    {
        LogManager::Log("ERR", "[OpusEncoder] opus_encode 失败: %d", encoded_bytes);
        return -1;
    }

    // 拷贝到输出参数
    output.assign(output_buffer_.data(), output_buffer_.data() + encoded_bytes);
    return static_cast<int>(encoded_bytes);
}
