#ifndef CAPTUREBACKEND_H
#define CAPTUREBACKEND_H

#include "CaptureCommon.h"

#include <memory>

// ========== 采集后端统一初始化上下文 ==========

struct BackendContext                                                    // 各采集后端初始化所需全部参数
{
    ID3D11Device* device{nullptr};                                       // D3D11 设备（无 GPU 时为空，GDI 不用）
    HMONITOR monitor{nullptr};                                           // 目标显示器句柄（Monitor 路线用）
    HWND window{nullptr};                                                // 目标窗口句柄（Window 路线用，Monitor 路线为空）
    RECT rect{0, 0, 0, 0};                                               // 采集矩形（GDI 路线用：显示器/窗口客户区）
    bool capture_cursor{false};                                          // 是否采集光标
    bool force_sdr{false};                                               // 是否强制 SDR（仅 WGC 用）
    bool client_area{false};                                             // 是否仅采集客户区（仅 WGC 窗口路线用）
};

// ========== 采集后端抽象基类 ==========

class CaptureBackend
{
public:
    virtual ~CaptureBackend() = default;

    virtual bool Init(const BackendContext& ctx) = 0;                    // 初始化后端
    virtual bool AcquireFrame() = 0;                                     // 采集一帧进内部缓冲
    virtual bool GetFrame(CaptureFrame& out) = 0;                        // 取出最新帧
    virtual bool IsActive() const = 0;                                   // 后端是否可用
    virtual void Shutdown() {}                                           // 释放资源（默认空实现）
    virtual DisplayCaptureMethod Kind() const = 0;                       // 后端对应的采集方法

    virtual uint32_t Width() const = 0;                                  // 帧宽度
    virtual uint32_t Height() const = 0;                                 // 帧高度
    virtual int Rotation() const { return 0; }                           // 旋转角度（仅 DXGI 有）
    virtual int MonitorX() const { return 0; }                           // 显示器 X 偏移（仅 DXGI 有）
    virtual int MonitorY() const { return 0; }                           // 显示器 Y 偏移（仅 DXGI 有）
    virtual void SetCursorHidden(bool hidden) {}                         // 光标隐藏（GDI/WGC 窗口路线用，默认空）
};

// ========== 工厂 ==========

// 按采集方法创建对应后端；Auto/非法值返回 nullptr
std::unique_ptr<CaptureBackend> CreateCaptureBackend(DisplayCaptureMethod method);

#endif // CAPTUREBACKEND_H
