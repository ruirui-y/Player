#ifndef IVIDEOENCODER_H
#define IVIDEOENCODER_H

#include <d3d11.h>
#include <cstdint>
#include <vector>
#include <functional>
#include "CaptureCommon.h"                                                                   // CursorInfo

// 视频编码器抽象接口
//
// ObsNvencEncoder（CPU sws_scale 色彩转换）和 ObsNvencEncoderFast（GPU VideoProcessor 色彩转换）
// 均实现此接口。StreamServer 通过此指针统一调用，运行时按 --fast 开关选择具体实现。
//
// 为什么用抽象基类而不是 std::variant / 函数指针表：
//   两个编码器的 Init/EncodeFrame/Flush/Release 签名完全一致，
//   用基类 + virtual 是 C++ 最直白的多态方式，调用方零分支，新增编码器只需继承。
class IVideoEncoder
{
public:
    virtual ~IVideoEncoder() = default;                                                      // 虚析构

    // 初始化编码器
    virtual bool Init(ID3D11Device* d3d_device, int width, int height,
                      int fps, int bitrate_kbps) = 0;                                        // 初始化

    // 编码一帧，返回 true 表示有码流输出
    // cursor/monitor_x/monitor_y：光标合成参数（GPU 路线可忽略，CPU 路线用于在 BGRA 缓冲上画光标）
    virtual bool EncodeFrame(ID3D11Texture2D* texture, uint64_t frame_index,
                             bool force_idr, std::vector<uint8_t>& out_data,
                             const CursorInfo* cursor = nullptr,
                             int monitor_x = 0, int monitor_y = 0) = 0;                      // 编码一帧

    // Flush 剩余帧（录制结束时调用）
    virtual void Flush(std::function<void(const std::vector<uint8_t>&)> on_packet) = 0;      // 刷新

    // 释放所有资源
    virtual void Release() = 0;                                                               // 释放资源

    // 是否就绪
    virtual bool IsReady() const = 0;                                                         // 是否就绪
};

#endif // IVIDEOENCODER_H
