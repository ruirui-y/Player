#ifndef OBSNVENCENCODER_H
#define OBSNVENCENCODER_H

#include <d3d11.h>
#include <cstdint>
#include <vector>
#include <functional>
#include "CaptureCommon.h"                                                                   // CursorInfo（光标合成用）
#include "IVideoEncoder.h"                                                                   // 编码器抽象接口

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

// 极简 NVENC 编码器 — 用 FFmpeg libavcodec 封装，不直接调 NVENC SDK
//
// 数据流（软件路径，与 FFmpeg nvenc.c 的 nvenc_upload_frame 一致）：
//   MonitorCapture → ID3D11Texture2D (GPU BGRA)
//     → CopyResource → staging 纹理 (CPU 可读)
//     → Map → CPU BGRA
//     → sws_scale → CPU NV12
//     → avcodec_send_frame → NVENC 硬件编码
//     → avcodec_receive_packet → H.264 码流
//
// 对比旧 NvencEncoder：
//   旧：直接调 nvEncCreateInputBuffer / nvEncLockInputBuffer / nvEncEncodePicture / nvEncLockBitstream
//   新：avcodec_send_frame / avcodec_receive_packet（FFmpeg 全部封装好了）
class ObsNvencEncoder : public IVideoEncoder
{
public:
    ObsNvencEncoder();                                                  // 构造
    ~ObsNvencEncoder();                                                 // 析构

    // 初始化编码器
    // d3d_device：D3D11 设备（用于 GPU→CPU 回读，与 MonitorCapture 共用）
    // width/height：编码尺寸
    // fps：帧率
    // bitrate_kbps：码率（如 10000 = 10Mbps）
    bool Init(ID3D11Device* d3d_device, int width, int height,
              int fps, int bitrate_kbps) override;                      // 初始化

    void Release() override;                                            // 释放所有资源

    // 编码一帧，返回 true 表示有码流输出
    // texture：MonitorCapture 采集到的 GPU 纹理
    // frame_index：帧序号（用于 PTS）
    // force_idr：是否强制 IDR 帧
    // out_data：输出 H.264 码流（可能为空，B 帧重排延迟）
    // cursor：光标信息（DXGI 路线需要外部合成光标，nullptr 表示不合成）
    // monitor_x/monitor_y：显示器屏幕偏移（将光标屏幕坐标转为纹理局部坐标）
    bool EncodeFrame(ID3D11Texture2D* texture, uint64_t frame_index,
                     bool force_idr, std::vector<uint8_t>& out_data,
                     const CursorInfo* cursor = nullptr,
                     int monitor_x = 0, int monitor_y = 0) override;   // 编码一帧

    // Flush 剩余帧（录制结束时调用）
    void Flush(std::function<void(const std::vector<uint8_t>&)> on_packet) override;  // 刷新

    bool IsReady() const override { return codec_ctx_ != nullptr; }

    bool SetBitrate(int bitrate_kbps) override;

private:
    bool CreateStagingTexture();                                        // 创建 CPU 可读的 staging 纹理
    bool CreateSwsContext();                                            // 创建 BGRA→NV12 色彩转换上下文
    bool GpuTextureToNv12(ID3D11Texture2D* texture,                     // GPU 纹理 → CPU NV12
                          const CursorInfo* cursor = nullptr,
                          int monitor_x = 0, int monitor_y = 0);
    void DrawCursorOnBuffer(uint8_t* bgra, int buf_w, int buf_h,       // 在 BGRA 缓冲上合成光标
                            const CursorInfo* cursor, int monitor_x, int monitor_y);

    // ---- D3D11（外部拥有 device，内部拥有 staging） ----
    ID3D11Device* d3d_device_{nullptr};
    ID3D11DeviceContext* d3d_ctx_{nullptr};
    ID3D11Texture2D* staging_texture_{nullptr};                         // GPU→CPU 回读用

    // ---- FFmpeg 编码器 ----
    const AVCodec* codec_{nullptr};
    AVCodecContext* codec_ctx_{nullptr};
    AVFrame* frame_{nullptr};                                           // NV12 输入帧
    AVPacket* packet_{nullptr};                                         // H.264 输出包

    // ---- 色彩转换 ----
    SwsContext* sws_ctx_{nullptr};                                      // BGRA→NV12
    uint8_t* nv12_data_{nullptr};                                       // NV12 数据缓冲
    int nv12_linesize_{0};                                              // NV12 行间距
    std::vector<uint8_t> bgra_buffer_;                                  // 可写 BGRA 缓冲（光标合成用）

    // ---- 参数 ----
    int width_{0};
    int height_{0};
    int fps_{60};
    int bitrate_kbps_{5000};
};

#endif // OBSNVENCENCODER_H
