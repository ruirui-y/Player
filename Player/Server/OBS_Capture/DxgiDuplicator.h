#ifndef DXGIDUPLICATOR_H
#define DXGIDUPLICATOR_H

#include "CaptureBackend.h"

#include <d3d11.h>
#include <dxgi1_2.h>

// ========== DXGI Desktop Duplication 采集器（GPU 路线） ==========
// 直接实现 CaptureBackend 接口；原 Init/UpdateFrame/GetTexture 降为 private 被 override 调用。

class DxgiDuplicator : public CaptureBackend
{
public:
    DxgiDuplicator();                                                                   // 构造
    ~DxgiDuplicator();                                                                  // 析构

    // ---- CaptureBackend 接口 ----
    bool Init(const BackendContext& ctx) override;                                      // 从 ctx.device / ctx.monitor 初始化
    bool AcquireFrame() override;                                                       // 更新帧（UpdateFrame），false=真错误需重建
    bool GetFrame(CaptureFrame& out) override;                                          // 取出最新帧纹理（GPU，含旋转）
    bool IsActive() const override;
    void Shutdown() override;                                                           // 释放 duplication 资源
    DisplayCaptureMethod Kind() const override { return DisplayCaptureMethod::Dxgi; }
    uint32_t Width() const override;
    uint32_t Height() const override;
    int Rotation() const override;
    int MonitorX() const override;
    int MonitorY() const override;

    // ---- 静态方法 ----
    static int GetMonitorIndex(HMONITOR monitor);                                       // 返回 DXGI output 索引，-1 表示不可用

private:
    bool CreateDuplication();                                                           // 创建 IDXGIOutputDuplication
    void ReleaseFrame();                                                                // 释放当前帧（ReleaseFrame）

    bool Init(ID3D11Device* device, HMONITOR monitor);                                  // 原初始化（Init(ctx) 内部调用）
    bool UpdateFrame(uint32_t timeout_ms = 0);                                          // 原帧更新（AcquireFrame 内部调用）
    ID3D11Texture2D* GetTexture() const;                                                // 取当前帧纹理（GetFrame 内部调用）

    ID3D11Device* device_{nullptr};                                                     // D3D11 设备（外部拥有，不可释放）
    ID3D11DeviceContext* context_{nullptr};                                             // D3D11 设备上下文（外部拥有）
    IDXGIOutputDuplication* duplication_{nullptr};                                      // DXGI Desktop Duplication 对象
    ID3D11Texture2D* target_texture_{nullptr};                                          // 私有目标纹理（ReleaseFrame 前拷贝桌面帧）
    uint32_t target_width_{0};                                                          // 目标纹理宽度（尺寸变化时重建）
    uint32_t target_height_{0};                                                         // 目标纹理高度

    HMONITOR monitor_{nullptr};                                                         // 目标显示器句柄
    int monitor_index_{-1};                                                             // DXGI output 索引
    uint32_t width_{0};                                                                 // 帧宽度
    uint32_t height_{0};                                                                // 帧高度
    int rotation_{0};                                                                   // 旋转角度
    int monitor_x_{0};                                                                  // 显示器 X 偏移
    int monitor_y_{0};                                                                  // 显示器 Y 偏移
    bool active_{false};                                                                // 是否正在采集
};

#endif // DXGIDUPLICATOR_H
