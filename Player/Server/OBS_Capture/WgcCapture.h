#ifndef WGCCAPTURE_H
#define WGCCAPTURE_H

#include "CaptureBackend.h"

#include <atomic>
#include <cstdint>
#include <mutex>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl.h>
#include <wrl/event.h>

#include <windows.foundation.h>
#include <windows.graphics.capture.h>

// IGraphicsCaptureItemInterop 在 um 目录下，不在 winrt 命名空间
#include <Windows.Graphics.Capture.Interop.h>

// ========== WGC 采集器（GPU 路线，支持显示器与窗口） ==========
// 直接实现 CaptureBackend 接口；InitMonitor/InitWindow 已合并为 Init(ctx)。

class WgcCapture : public CaptureBackend
{
public:
    WgcCapture();                                                                               // 构造
    ~WgcCapture();                                                                              // 析构

    // ---- CaptureBackend 接口 ----
    bool Init(const BackendContext& ctx) override;                                              // 合并 Monitor/Window 两条路线（按 ctx.window 区分）
    bool AcquireFrame() override;                                                               // 泵一帧（Tick），返回 backend 是否活跃
    bool GetFrame(CaptureFrame& out) override;                                                  // 取出最新帧纹理（GPU）
    bool IsActive() const override;
    void Shutdown() override;                                                                   // 停止采集并释放资源
    DisplayCaptureMethod Kind() const override { return DisplayCaptureMethod::Wgc; }
    uint32_t Width() const override;
    uint32_t Height() const override;
    void SetCursorHidden(bool hidden) override;                                                 // 内部调 ShowCursor(!hidden)

    // ---- 仍需公开的部分 ----
    static bool IsSupported();                                                                  // 系统是否支持 WGC
    void OnFrameArrived();                                                                      // FrameArrived 回调（供 handler 类调用）

private:
    // 从 ID3D11Device 创建 WinRT IDirect3DDevice
    bool CreateDirect3DDeviceFromD3D11(ID3D11Device* d3d11_device);

    // 从 HMONITOR 创建 GraphicsCaptureItem
    bool CreateItemForMonitor(HMONITOR monitor);

    // 从 HWND 创建 GraphicsCaptureItem
    bool CreateItemForWindow(HWND window);

    // 创建 FramePool 和 CaptureSession 并启动
    bool StartCapture();

    // 释放帧纹理
    void ReleaseFrame();

    // 每帧泵（原 Tick，现由 AcquireFrame 调用）
    bool Tick();

    // 动态切换光标显隐（SetCursorHidden 内部调用）
    bool ShowCursor(bool visible);

    // 取当前帧纹理（GetFrame 内部调用）
    ID3D11Texture2D* GetTexture() const;

    // ---- D3D11 资源 ----
    ID3D11Device* device_{nullptr};                                                             // D3D11 设备（外部拥有）
    IDXGIDevice* dxgi_device_{nullptr};                                                         // DXGI 设备（内部拥有）

    // ---- WinRT 资源 ----
    ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice* direct3d_device_{nullptr};
    ABI::Windows::Graphics::Capture::IGraphicsCaptureItem* item_{nullptr};
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool* frame_pool_{nullptr};
    ABI::Windows::Graphics::Capture::IGraphicsCaptureSession* session_{nullptr};
    ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2* session2_{nullptr};              // put_IsCursorCaptureEnabled 在此接口

    // ---- 事件 token ----
    EventRegistrationToken frame_arrived_token_{};
    EventRegistrationToken closed_token_{};

    // ---- 帧数据 ----
    ID3D11Texture2D* current_texture_{nullptr};                                                 // 当前帧纹理
    mutable std::mutex texture_mutex_;                                                          // 保护 current_texture_ 的跨线程访问（const 函数可锁）
    std::atomic<bool> new_frame_arrived_{false};                                                // FrameArrived 回调设置的标志
    std::atomic<bool> closed_{false};                                                           // CaptureItem 是否已关闭

    // ---- 配置 ----
    bool cursor_{false};                                                                        // 是否采集光标
    bool force_sdr_{false};                                                                     // 是否强制 SDR
    bool client_area_{false};                                                                   // 窗口采集是否仅客户区
    bool active_{false};                                                                        // 是否正在采集
    uint32_t width_{0};                                                                         // 帧宽度
    uint32_t height_{0};                                                                        // 帧高度

    bool first_frame_diagnostic_logged_{false};                                                  // 是否已输出首帧成功诊断
    bool first_frame_error_logged_{false};                                                       // 是否已输出首帧错误诊断

    // ---- 事件回调对象 ----
    Microsoft::WRL::ComPtr<IUnknown> frame_arrived_handler_;                                    // FrameArrived 回调包装
};

#endif // WGCCAPTURE_H
