#ifndef OBSNVENCENCODERFAST_H
#define OBSNVENCENCODERFAST_H

#include <d3d11.h>
#include <cstdint>
#include <vector>
#include <functional>
#include "IVideoEncoder.h"                                                                   // 编码器抽象接口

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
}

// 高性能 NVENC 编码器 — D3D11 硬件转换版
//
// 核心优势：
// 利用显卡自带的 Video Processor 在显存内部将 BGRA 转为 NV12，
// 消除了 CPU 软件转换的瓶颈，大幅降低 CPU 占用。
//
// 与 ObsNvencEncoder 的区别：
//   旧版：GPU 纹理 → CopyResource → CPU BGRA → sws_scale → CPU NV12 → NVENC
//   本版：GPU 纹理 → VideoProcessorBlt → GPU NV12 → CopyResource → CPU NV12 → NVENC
//   省掉了 sws_scale 的 CPU 开销，色彩转换由 GPU 固定功能单元完成

class ObsNvencEncoderFast : public IVideoEncoder {
public:
    ObsNvencEncoderFast();
    ~ObsNvencEncoderFast();

    // 初始化编码器
    // d3d_device：D3D11 设备（需与捕获源共用）
    bool Init(ID3D11Device* d3d_device, int width, int height, int fps, int bitrate_kbps) override;

    void Release() override;

    // 编码一帧
    // texture：MonitorCapture 采集到的 GPU 纹理 (BGRA)
    // cursor/monitor_x/monitor_y：光标合成参数，GPU 路线不使用（已改用客户端本地光标）
    bool EncodeFrame(ID3D11Texture2D* texture, uint64_t frame_index,
        bool force_idr, std::vector<uint8_t>& out_data,
        const CursorInfo* cursor = nullptr,
        int monitor_x = 0, int monitor_y = 0) override;

    void Flush(std::function<void(const std::vector<uint8_t>&)> on_packet) override;

    bool IsReady() const override { return codec_ctx_ != nullptr; }

    bool SetBitrate(int bitrate_kbps) override;

private:
    // 初始化 D3D11 视频处理器（用于硬件格式转换）
    bool InitVideoProcessor();

    // 执行 GPU 格式转换 (BGRA -> NV12)
    bool ConvertBgraToNv12Gpu(ID3D11Texture2D* src_bgra);

    // ---- D3D11 基础资源 ----
    ID3D11Device* d3d_device_{ nullptr };
    ID3D11DeviceContext* d3d_ctx_{ nullptr };

    // ---- D3D11 视频处理器资源 ----
    ID3D11VideoDevice* video_device_{ nullptr };
    ID3D11VideoContext* video_ctx_{ nullptr };
    ID3D11VideoProcessor* video_processor_{ nullptr };
    ID3D11VideoProcessorEnumerator* video_proc_enum_{ nullptr };

    // ---- 纹理资源 ----
    ID3D11Texture2D* nv12_texture_{ nullptr };      // GPU 转换目标
    ID3D11Texture2D* nv12_staging_{ nullptr };      // CPU 读取用的中转纹理

    // ---- FFmpeg 编码器 ----
    const AVCodec* codec_{ nullptr };
    AVCodecContext* codec_ctx_{ nullptr };
    AVFrame* frame_{ nullptr };
    AVPacket* packet_{ nullptr };

    // ---- 参数 ----
    int width_{ 0 };
    int height_{ 0 };
    int fps_{ 60 };
    int bitrate_kbps_{ 5000 };
};

#endif // OBSNVENCENCODERFAST_H