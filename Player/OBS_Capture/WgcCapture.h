#ifndef WGCCAPTURE_H
#define WGCCAPTURE_H

#include "CaptureCommon.h"

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

class WgcCapture
{
public:
    WgcCapture();                                                       // 构造
    ~WgcCapture();                                                      // 析构

    // 显示器采集初始化
    bool InitMonitor(ID3D11Device* device, HMONITOR monitor, bool cursor, bool force_sdr);

    // 窗口采集初始化
    bool InitWindow(ID3D11Device* device, HWND window, bool client_area, bool cursor, bool force_sdr);

    void Shutdown();                                                    // 停止采集并释放资源

    // 每帧调用，检查是否有新帧到达并获取
    bool Tick();

    // 获取当前帧纹理（不拥有所有权，调用方不可释放）
    ID3D11Texture2D* GetTexture() const;

    uint32_t Width() const;                                             // 帧宽度
    uint32_t Height() const;                                            // 帧高度
    bool IsActive() const;                                              // 采集是否活跃
    bool ShowCursor(bool visible);                                      // 动态切换光标显隐

    // 静态检测：系统是否支持 WGC
    static bool IsSupported();

    // FrameArrived 回调（public，供 handler 类调用）
    void OnFrameArrived();

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

    // ---- D3D11 资源 ----
    ID3D11Device* device_{nullptr};                                     // D3D11 设备（外部拥有）
    IDXGIDevice* dxgi_device_{nullptr};                                 // DXGI 设备（内部拥有）

    // ---- WinRT 资源 ----
    ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice* direct3d_device_{nullptr};
    ABI::Windows::Graphics::Capture::IGraphicsCaptureItem* item_{nullptr};
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool* frame_pool_{nullptr};
    ABI::Windows::Graphics::Capture::IGraphicsCaptureSession* session_{nullptr};
    ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2* session2_{nullptr};  // put_IsCursorCaptureEnabled 在此接口

    // ---- 事件 token ----
    EventRegistrationToken frame_arrived_token_{};
    EventRegistrationToken closed_token_{};

    // ---- 帧数据 ----
    ID3D11Texture2D* current_texture_{nullptr};                         // 当前帧纹理
    mutable std::mutex texture_mutex_;                                  // 保护 current_texture_ 的跨线程访问（const 函数可锁）
    std::atomic<bool> new_frame_arrived_{false};                        // FrameArrived 回调设置的标志
    std::atomic<bool> closed_{false};                                   // CaptureItem 是否已关闭

    // ---- 配置 ----
    bool cursor_{false};                                                // 是否采集光标
    bool force_sdr_{false};                                             // 是否强制 SDR
    bool client_area_{false};                                           // 窗口采集是否仅客户区
    bool active_{false};                                                // 是否正在采集
    uint32_t width_{0};                                                 // 帧宽度
    uint32_t height_{0};                                                // 帧高度

    // ---- 事件回调对象 ----
    Microsoft::WRL::ComPtr<IUnknown> frame_arrived_handler_;            // FrameArrived 回调包装
};

#endif // WGCCAPTURE_H
