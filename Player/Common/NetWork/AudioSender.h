#ifndef AUDIOSENDER_H
#define AUDIOSENDER_H

#include <cstdint>

#include <WinSock2.h>
#include <WS2tcpip.h>

// UDP 音频发送器
// 职责：将 Opus 编码后的音频帧通过 UDP 发送
// 复用 NalUnit.h 的分包结构，音频包通常 <400 字节，单包即可送达
class AudioSender
{
public:
    AudioSender();                                                  // 构造
    ~AudioSender();                                                 // 析构

    // 初始化 UDP socket
    // dest_ip：目标地址
    // dest_port：目标端口（如 47997）
    bool Init(const char* dest_ip, uint16_t dest_port);             // 初始化

    void Close();                                                   // 关闭 socket

    // 发送一帧 Opus 数据
    // frame_data：Opus 压缩数据
    // frame_size：数据大小
    // frame_index：帧序号
    // timestamp：时间戳（毫秒）
    void SendFrame(const uint8_t* frame_data, int frame_size,
                   uint16_t frame_index, uint32_t timestamp);       // 发送一帧

    bool IsReady() const { return sock_ != INVALID_SOCKET; }        // 是否就绪

private:
    SOCKET sock_{INVALID_SOCKET};                                   // UDP socket
    sockaddr_in dest_addr_{};                                       // 目标地址
    bool wsa_started_{false};                                       // WSA 是否已启动
};

#endif // AUDIOSENDER_H
