#ifndef VIDEORECEIVER_H
#define VIDEORECEIVER_H

// 必须在任何 Windows 头文件之前定义，阻止 min/max 宏污染 std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <atomic>
#include <thread>
#include <string>
#include <functional>
#include <chrono>
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "NalUnit.h"
#include "NetworkStats.h"

struct AVPacket;

template<typename T> class SafeQueue;

// ---- 类概述 ----
// UDP 视频接收器
// 开一个线程 recvfrom 收包，用 NalReassembler 组帧（含 FEC 恢复）
// 组帧完成后包装成 AVPacket* 推入 SafeQueue<AVPacket*>
// 下游 VideoDecoder 直接消费队列，无需改动
// 丢包统计 + IDR 请求：跟踪 frame_index 连续性检测丢帧，连续丢 ≥ MAX_CONSECUTIVE_LOST 帧时
// 触发 idr_request_callback_，由上层通过控制信道发送 IDR 请求到服务端
class VideoReceiver
{
public:
    VideoReceiver();                                                                  // 构造
    ~VideoReceiver();                                                                 // 析构

    // ---- 生命周期 ----
    bool Init(uint16_t listen_port, SafeQueue<AVPacket*>* packet_queue);              // 初始化并绑定 UDP socket
    void Start();                                                                     // 启动接收线程
    void Stop();                                                                      // 停止接收线程
    bool IsRunning() const { return running_; }                                       // 是否正在运行

    // ---- 状态查询 ----
    std::string GetSenderIP() const { return sender_ip_; }                            // 发送方 IP（首包到达前为空串）
    int GetReceiveFps() const { return receive_fps_.load(); }                         // 当前接收帧率（帧/秒，原子读）

    // ---- 回调注册（均在接收线程内触发，回调体需注意线程安全）----
    // 首包到达时触发：上报发送方 IP，上层据此自动建立控制信道
    void SetSenderIPCallback(std::function<void(const std::string&)> cb)
    { sender_ip_cb_ = std::move(cb); }

    // 需要请求 IDR 时触发：上层应通过控制信道发送 { "type": "request_idr" } 到服务端
    void SetIdrRequestCallback(std::function<void()> callback)
    {
        idr_request_callback_ = std::move(callback);
    }

    // 每秒触发一次：上报接收统计（帧率 / 丢包率 / FEC / 带宽估算）
    void SetStatsCallback(std::function<void(const NetworkStats&)> callback)
    {
        stats_callback_ = std::move(callback);
    }

    // ---- 丢包统计查询 ----
    uint64_t TotalPackets() const { return total_packets_; }                          // 收到的 UDP 包总数
    uint64_t TotalFrames() const { return total_frames_; }                            // 组帧完成的帧总数
    int TotalLostFrames() const { return total_lost_frames_; }                        // 总丢帧数
    int FecRecoveredFrames() const { return fec_recovered_frames_; }                  // FEC 恢复的帧数（累计）

private:
    // ---- 接收线程 ----
    void ReceiveLoop();                                                               // 接收线程主循环

    // ---- ReceiveLoop 子阶段（从 ReceiveLoop 拆分，便于独立测试）----
    void RecordSenderIP(const sockaddr_in& from_addr);                                // 首包：记录发送方 IP + 触发回调
    void HandleFrameLossAndIdr(uint16_t current_frame);                               // 丢包检测 + 连续丢帧请求 IDR
    void ReportStats();                                                               // 每秒网络统计上报
    void BuildAndEnqueuePacket(const std::vector<uint8_t>& frame_data);               // 组 AVPacket + 队列堆积丢帧 + 推入队列

    // ---- 网络资源 ----
    SOCKET sock_{INVALID_SOCKET};                                                     // UDP socket
    sockaddr_in listen_addr_{};                                                       // 监听地址
    bool wsa_started_{false};                                                         // WSA 是否已启动

    // ---- 线程与队列 ----
    std::thread recv_thread_;                                                         // 接收线程
    std::atomic<bool> running_{false};                                                // 运行标志
    SafeQueue<AVPacket*>* packet_queue_{nullptr};                                     // 输出队列（外部拥有）
    NalReassembler reassembler_;                                                      // 组帧器（含 FEC 恢复）

    // ---- 发送方地址 ----
    std::string sender_ip_;                                                           // 发送方 IP（首包获取，用于自动连接控制信道）

    // ---- 接收统计 ----
    uint64_t total_packets_{0};                                                       // 收到的 UDP 包总数
    uint64_t total_frames_{0};                                                        // 组帧完成的帧总数

    // ---- 丢包检测 ----
    uint16_t expected_frame_index_{0};                                                // 期望的下一个 frame_index（0=未初始化）
    int consecutive_lost_frames_{0};                                                  // 连续丢失的帧数
    int total_lost_frames_{0};                                                        // 总丢帧数
    int fec_recovered_frames_{0};                                                     // FEC 恢复的帧数（累计）

    // ---- IDR 请求 ----
    std::function<void()> idr_request_callback_;                                      // IDR 请求回调
    std::chrono::steady_clock::time_point last_idr_request_time_;                     // 上次 IDR 请求时间

    // ---- 网络统计上报 ----
    std::function<void(const NetworkStats&)> stats_callback_;                         // 统计上报回调
    std::chrono::steady_clock::time_point last_stats_time_;                           // 上次统计时间
    uint64_t last_stats_packets_{0};                                                  // 上次统计时的包数
    uint64_t last_stats_frames_{0};                                                   // 上次统计时的帧数
    uint64_t last_stats_bytes_{0};                                                    // 上次统计时的字节数
    int last_fec_recovered_{0};                                                       // 上次统计时的 FEC 恢复数
    int last_fec_failed_{0};                                                          // 上次统计时的 FEC 失败数
    uint64_t total_bytes_{0};                                                         // 累计接收字节数
    int queue_dropped_frames_{0};                                                     // 因客户端堆积主动丢弃的完整帧数
    std::chrono::steady_clock::time_point last_queue_drop_log_time_{};                // 上次队列丢帧日志时间

    // ---- OSD 统计 ----
    std::atomic<int> receive_fps_{0};                                                 // 接收帧率

    // ---- 回调句柄 ----
    std::function<void(const std::string&)> sender_ip_cb_;                            // 首包回调
};

#endif // VIDEORECEIVER_H
