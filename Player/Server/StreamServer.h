#ifndef STREAMSERVER_H
#define STREAMSERVER_H

#include <cstdint>
#include <atomic>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
class MonitorCapture;
class ObsNvencEncoder;
class VideoSender;

// 服务器模式：桌面采集 → NVENC 编码 → UDP 发送
// 不依赖 Qt 事件循环，直接在 main() 中阻塞运行
// 用法：Player.exe --server --port 47998 --monitor 1 --ip 127.0.0.1
class StreamServer
{
public:
    StreamServer();                                                                                   // 构造
    ~StreamServer();                                                                                  // 析构

    // 初始化所有组件
    // port：UDP 目标端口
    // monitor_index：显示器索引（0=主显示器，1=副显示器）
    // dest_ip：目标 IP（默认 127.0.0.1）
    // fps：采集/编码帧率
    // bitrate_kbps：码率（如 10000 = 10Mbps）
    bool Init(uint16_t port, int monitor_index,                                                       // 初始化
              const char* dest_ip = "127.0.0.1",
              int fps = 60, int bitrate_kbps = 10000);

    void Run();                                                                                       // 主循环（阻塞），按帧率采集→编码→发送
    void Stop();                                                                                      // 停止主循环

private:
    // ---- D3D11 ----
    ID3D11Device* d3d_device_{nullptr};                                                               // D3D11 设备
    ID3D11DeviceContext* d3d_ctx_{nullptr};                                                           // D3D11 设备上下文

    // ---- 组件 ----
    MonitorCapture* capture_{nullptr};                                                                // 桌面采集器
    ObsNvencEncoder* encoder_{nullptr};                                                               // NVENC 编码器
    VideoSender* sender_{nullptr};                                                                    // UDP 发送器

    // ---- GDI 路线上传纹理 ----
    ID3D11Texture2D* upload_tex_{nullptr};                                                            // CPU→GPU 上传用纹理（GDI 路线）

    // ---- 参数 ----
    int fps_{60};                                                                                     // 帧率
    int width_{0};                                                                                    // 画面宽度
    int height_{0};                                                                                   // 画面高度

    std::atomic<bool> running_{false};                                                                // 运行标志
};

#endif // STREAMSERVER_H
