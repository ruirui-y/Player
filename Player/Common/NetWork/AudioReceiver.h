#ifndef AUDIORECEIVER_H
#define AUDIORECEIVER_H

#include <cstdint>
#include <atomic>
#include <thread>

#include <WinSock2.h>
#include <WS2tcpip.h>

#include "Common/NetWork/NalUnit.h"

struct AVPacket;
template<typename T> class SafeQueue;

// UDP 音频接收器
// 职责：开一个线程 recvfrom 收包，用 NalReassembler 组帧
// 组帧完成后包装成 AVPacket* 推入 SafeQueue<AVPacket*>
// 下游 AudioDecoder 直接消费队列
class AudioReceiver
{
public:
    AudioReceiver();                                                // 构造
    ~AudioReceiver();                                               // 析构

    // 初始化并绑定 UDP socket
    // listen_port：监听端口（如 47997）
    // packet_queue：输出队列，组帧后的 AVPacket* 推入此处
    bool Init(uint16_t listen_port, SafeQueue<AVPacket*>* packet_queue);   // 初始化

    void Start();                                                   // 启动接收线程
    void Stop();                                                    // 停止接收线程

    bool IsRunning() const { return running_; }                     // 是否运行中

private:
    void ReceiveLoop();                                             // 接收线程主循环

    SOCKET sock_{INVALID_SOCKET};                                   // UDP socket
    sockaddr_in listen_addr_{};                                     // 监听地址
    bool wsa_started_{false};                                       // WSA 是否已启动

    std::thread recv_thread_;                                       // 接收线程
    std::atomic<bool> running_{false};                              // 运行标志

    SafeQueue<AVPacket*>* packet_queue_{nullptr};                   // 输出队列（外部拥有）
    NalReassembler reassembler_;                                    // 组帧器

    // ---- 统计 ----
    uint64_t total_packets_{0};                                     // 收到的 UDP 包总数
    uint64_t total_frames_{0};                                      // 组帧完成的帧总数
};

#endif // AUDIORECEIVER_H
