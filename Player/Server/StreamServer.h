#ifndef STREAMSERVER_H
#define STREAMSERVER_H

#include <cstdint>
#include <atomic>
#include <vector>
#include <chrono>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
class MonitorCapture;
class IVideoEncoder;
class VideoSender;
class InputTransportServer;
class InputInjector;
class WasapiCapture;
class OpusAudioEncoder;
class AudioSender;
class BitrateController;
struct CaptureFrame;

// 服务器模式：桌面采集 → NVENC 编码 → UDP 发送
// 第三阶段：TCP 控制信道接收输入事件 → SendInput 注入
// 不依赖 Qt 事件循环，直接在 main() 中阻塞运行
// 用法：Player.exe --server --port 47998 --monitor 1 --ip 127.0.0.1 --ctrl-port 47989
class StreamServer
{
public:
    // ---- 构造 / 析构 / 公开接口 ----
    StreamServer();                                                                                     // 构造
    ~StreamServer();                                                                                    // 析构

    // 初始化所有组件
    // port：UDP 目标端口
    // monitor_index：显示器索引（0=主显示器，1=副显示器）
    // dest_ip：目标 IP（默认 127.0.0.1）
    // fps：采集/编码帧率
    // bitrate_kbps：码率（如 10000 = 10Mbps）
    // ctrl_port：TCP 控制信道端口（如 47989）
    // use_fast：true=GPU 硬件色彩转换(ObsNvencEncoderFast)，false=CPU sws_scale(ObsNvencEncoder)
    bool Init(uint16_t port, int monitor_index,                                                         // 初始化
              const char* dest_ip = "127.0.0.1",
              int fps = 60, int bitrate_kbps = 10000,
              uint16_t ctrl_port = 47989,
              bool use_fast = false,
              uint16_t audio_port = 47997);

    void Run();                                                                                         // 主循环（阻塞），按帧率采集→编码→发送
    void Stop();                                                                                        // 停止主循环

private:
    // ---- 方法 ----
    void OnAudioData(const int16_t* pcm_data, int sample_count, uint32_t timestamp_ms);                 // 音频回调：缓冲+编码+发送

    // ---- 初始化子阶段（从 Init 拆分，便于独立测试与失败回滚）----
    bool InitD3D11();                                                                                   // 创建 D3D11 设备
    bool InitCaptureStage(int monitor_index);                                                           // 枚举显示器 + 初始化采集器 + 等待首帧
    bool InitEncoder(bool use_fast, int bitrate_kbps);                                                  // 创建并初始化 NVENC 编码器
    bool InitSender(const char* dest_ip, uint16_t port);                                                // 初始化 UDP 视频发送器
    bool InitUploadTexture();                                                                           // GDI 路线创建 CPU→GPU 上传纹理
    bool InitInputChannel(uint16_t ctrl_port);                                                          // TCP 控制信道 + 回调绑定 + 启动
    void InitAudio(const char* dest_ip, uint16_t audio_port);                                           // 音频链路（非致命）
    void Shutdown();                                                                                    // 逆序释放所有资源（供析构与 Init 失败回滚复用）

    // ---- 主循环子阶段（从 Run 拆分，逻辑解耦）----
    bool CaptureOneFrame(CaptureFrame& frame);                                                          // 采集一帧，失败记日志并返回 false
    ID3D11Texture2D* ResolveTexture(CaptureFrame& frame);                                               // GPU 直取 或 GDI 上传，返回待编码纹理
    bool EncodeAndSend(ID3D11Texture2D* tex, uint64_t frame_index);                                      // 取光标 → 编码 → UDP 发送
    void SleepRemainder(std::chrono::steady_clock::time_point frame_start,                              // 帧率限制：补齐到 frame_duration
                        std::chrono::microseconds frame_duration);

    // ---- 成员变量 ----
    // D3D11
    ID3D11Device* d3d_device_{nullptr};                                                                 // D3D11 设备
    ID3D11DeviceContext* d3d_ctx_{nullptr};                                                             // D3D11 设备上下文

    // 视频组件
    MonitorCapture* capture_{nullptr};                                                                  // 桌面采集器
    IVideoEncoder* encoder_{nullptr};                                                                   // NVENC 编码器（指向 ObsNvencEncoder 或 ObsNvencEncoderFast）
    VideoSender* sender_{nullptr};                                                                      // UDP 发送器

    // 第三阶段：输入控制组件
    InputTransportServer* input_server_{nullptr};                                                       // TCP 输入接收器
    InputInjector* input_injector_{nullptr};                                                            // 输入注入器

    // GDI 路线上传纹理
    ID3D11Texture2D* upload_tex_{nullptr};                                                              // CPU→GPU 上传用纹理（GDI 路线）
    bool capture_gpu_{false};                                                                           // 首帧是否 GPU 纹理（决定是否创建上传纹理）

    // 第四阶段：音频组件
    WasapiCapture* wasapi_capture_{nullptr};                                                            // WASAPI 环回采集器
    OpusAudioEncoder* opus_encoder_{nullptr};                                                           // Opus 编码器
    AudioSender* audio_sender_{nullptr};                                                                // UDP 音频发送器

    std::vector<int16_t> audio_pcm_buffer_;                                                             // PCM 累积缓冲区（凑帧用）
    uint16_t audio_frame_index_{0};                                                                     // 音频帧序号
    uint32_t audio_timestamp_ms_{0};                                                                    // 音频时间戳（毫秒）

    // 参数
    int fps_{60};                                                                                       // 帧率
    int width_{0};                                                                                      // 画面宽度
    int height_{0};                                                                                     // 画面高度

    std::atomic<bool> running_{false};                                                                  // 运行标志
    int consecutive_failures_{0};                                                                       // 连续采集失败计数
    std::atomic<bool> force_next_idr_{false};                                                           // 收到 IDR 请求后下一帧强制 IDR
    BitrateController* bitrate_ctrl_{nullptr};                                                          // 码率自适应控制器

public:
    // UI 反馈（原子变量，UI 线程可安全读取）—— 需在类末统一放置
    std::atomic<uint64_t> frame_count_{0};                                                              // 编码帧计数
    std::atomic<bool> client_connected_{false};                                                         // 是否已有客户端连接
    std::atomic<int> current_bitrate_{0};                                                               // 当前实际码率（自适应后变化）
};

#endif // STREAMSERVER_H
