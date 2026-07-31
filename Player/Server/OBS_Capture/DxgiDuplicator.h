#ifndef DXGIDUPLICATOR_H
#define DXGIDUPLICATOR_H

#include "CaptureCommon.h"

#include <d3d11.h>
#include <dxgi1_2.h>

class DxgiDuplicator
{
public:
    DxgiDuplicator();                                                   // 构造
    ~DxgiDuplicator();                                                  // 析构

    // 初始化，传入已创建的 D3D11 设备和目标显示器句柄
    bool Init(ID3D11Device* device, HMONITOR monitor);

    void Shutdown();                                                    // 释放 duplication 资源

    // 更新帧，返回 true 表示有新帧。timeout_ms 为 AcquireNextFrame 超时
    bool UpdateFrame(uint32_t timeout_ms = 0);

    // 获取当前帧纹理（不拥有所有权，调用方不可释放）
    ID3D11Texture2D* GetTexture() const;

    uint32_t Width() const;                                             // 帧宽度
    uint32_t Height() const;                                            // 帧高度
    int Rotation() const;                                               // 旋转角度（0/90/180/270）
    int MonitorX() const;                                               // 显示器在虚拟桌面中的 X 偏移
    int MonitorY() const;                                               // 显示器在虚拟桌面中的 Y 偏移
    bool IsActive() const;                                              // duplication 是否有效

    // 检查显示器是否可被 DXGI 采集（静态方法，不需要实例）
    static int GetMonitorIndex(HMONITOR monitor);                       // 返回 DXGI output 索引，-1 表示不可用

private:
    bool CreateDuplication();                                           // 创建 IDXGIOutputDuplication
    void ReleaseFrame();                                                // 释放当前帧（ReleaseFrame）

    ID3D11Device* device_{nullptr};                                     // D3D11 设备（外部拥有，不可释放）
    ID3D11DeviceContext* context_{nullptr};                             // D3D11 设备上下文（外部拥有）
    IDXGIOutputDuplication* duplication_{nullptr};                      // DXGI Desktop Duplication 对象
    ID3D11Texture2D* target_texture_{nullptr};                          // 私有目标纹理（ReleaseFrame 前拷贝桌面帧）
    uint32_t target_width_{0};                                          // 目标纹理宽度（尺寸变化时重建）
    uint32_t target_height_{0};                                         // 目标纹理高度

    HMONITOR monitor_{nullptr};                                         // 目标显示器句柄
    int monitor_index_{-1};                                             // DXGI output 索引
    uint32_t width_{0};                                                 // 帧宽度
    uint32_t height_{0};                                                // 帧高度
    int rotation_{0};                                                   // 旋转角度
    int monitor_x_{0};                                                  // 显示器 X 偏移
    int monitor_y_{0};                                                  // 显示器 Y 偏移
    bool active_{false};                                                // 是否正在采集
};

#endif // DXGIDUPLICATOR_H
