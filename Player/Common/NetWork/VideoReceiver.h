#ifndef VIDEORECEIVER_H
#define VIDEORECEIVER_H

#include <cstdint>
#include <atomic>
#include <thread>
#include <string>
#include <functional>
#include <chrono>
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "NalUnit.h"

struct AVPacket;

template<typename T> class SafeQueue;

// UDP 视频接收器
// 开一个线程 recvfrom 收包，用 NalReassembler 组帧（含 FEC 恢复）
// 组帧完成后包装成 AVPacket* 推入 SafeQueue<AVPacket*>
// 下游 VideoDecoder 直接消费队列，无需改动
//
// 丢包统计 + IDR 请求：
//   跟踪 frame_index 连续性检测丢帧，连续丢 ≥ MAX_CONSECUTIVE_LOST 帧时
//   触发 OnIdrRequested 回调，由上层通过控制信道发送 IDR 请求到服务端
class VideoReceiver
{
public:
    VideoReceiver();                                        // 构造
    ~VideoReceiver();                                       // 析构

    // 初始化并绑定 UDP socket
    // listen_port：监听端口（如 47998）
    // packet_queue：输出队列，组帧后的 AVPacket* 推入此处
    bool Init(uint16_t listen_port, SafeQueue<AVPacket*>* packet_queue);    // 初始化

    void Start();                                           // 启动接收线程
    void Stop();                                            // 停止接收线程

    bool IsRunning() const { return running_; }             // 是否运���中

    // 获取视频流发送方 IP（收到第一个 UDP 包后才有值）
    // 返回字符串如 "192.168.31.142"，未收到包时返回空字符串
    std::string GetSenderIP() const { return sender_ip_; } // 获取发送方 IP

    // IDR 请求回调（客户端上层设置，当需要请求 IDR 时触发）
    // 上层应通过控制信道发送 { "type": "request_idr" } JSON 到服务端
    void SetIdrRequestCallback(std::function<void()> callback)
    {
        idr_request_callback_ = std::move(callback);
    }

    // 丢包统计
    uint64_t TotalPackets() const { return total_packets_; }
    uint64_t TotalFrames() const { return total_frames_; }
    int TotalLostFrames() const { return total_lost_frames_; }
    int FecRecoveredFrames() const { return fec_recovered_frames_; }

private:
    void ReceiveLoop();                                     // 接收线程主循环

    SOCKET sock_{INVALID_SOCKET};                           // UDP socket
    sockaddr_in listen_addr_{};                             // 监听地址
    bool wsa_started_{false};                               // WSA 是否已启动

    std::thread recv_thread_;                               // 接收线程
    std::atomic<bool> running_{false};                      // 运行标志

    SafeQueue<AVPacket*>* packet_queue_{nullptr};           // 输出队列（外部拥有）
    NalReassembler reassembler_;                            // 组帧器（含 FEC 恢复）

    // 发送方地址（从第一个 UDP 包获取，用于自动连接控制信道）
    std::string sender_ip_;                                 // 发送方 IP 地址

    // 统计
    uint64_t total_packets_{0};                             // 收到的 UDP 包总数
    uint64_t total_frames_{0};                              // 组帧完成的帧总数

    // ---- 丢包检测 ----
    uint16_t expected_frame_index_{0};                      // 期望的下一个 frame_index（0=未初始化）
    int consecutive_lost_frames_{0};                        // 连续丢失的帧数
    int total_lost_frames_{0};                              // 总丢帧数
    int fec_recovered_frames_{0};                           // FEC 恢复的帧数（累计）

    // ---- IDR 请求 ----
    std::function<void()> idr_request_callback_;            // IDR 请求回调
    std::chrono::steady_clock::time_point last_idr_request_time_;  // 上次 IDR 请求时间
};

#endif // VIDEORECEIVER_H
