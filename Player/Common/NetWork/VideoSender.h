#ifndef VIDEOSENDER_H
#define VIDEOSENDER_H

#include <cstdint>
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "NalUnit.h"

#pragma comment(lib, "ws2_32.lib")

// UDP 视频发送器
// 将 ObsNvencEncoder 输出的 H.264 帧分片后通过 UDP 发送
// 使用原生 Winsock2，避免 Qt 网络层的额外延迟
class VideoSender
{
public:
    VideoSender();                                          // 构造
    ~VideoSender();                                         // 析构

    // 初始化 UDP socket
    // dest_ip：目标地址（如 "127.0.0.1"）
    // dest_port：目标端口（如 47998）
    bool Init(const char* dest_ip, uint16_t dest_port);     // 初始化

    void Close();                                           // 关闭 socket

    // 发送一帧 H.264 数据
    // frame_data：H.264 Annex B 数据
    // frame_size：数据大小
    // frame_index：帧序号
    // timestamp：时间戳（毫秒）
    // is_keyframe：是否关键帧
    void SendFrame(const uint8_t* frame_data, int frame_size,
                   uint16_t frame_index, uint32_t timestamp,
                   bool is_keyframe);                       // 分片并发送

    bool IsReady() const { return sock_ != INVALID_SOCKET; }   // 是否就绪

private:
    SOCKET sock_{INVALID_SOCKET};                           // UDP socket
    sockaddr_in dest_addr_{};                               // 目标地址
    bool wsa_started_{false};                               // WSA 是否已启动
};

#endif // VIDEOSENDER_H
