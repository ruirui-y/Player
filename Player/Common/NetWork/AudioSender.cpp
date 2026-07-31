#include "AudioSender.h"
#include "Common/NetWork/NalUnit.h"

#include <QDebug>

// 初始化 Winsock 并创建 UDP socket
bool AudioSender::Init(const char* dest_ip, uint16_t dest_port)
{
    // ---- 第一步：初始化 Winsock ----
    WSADATA wsa_data;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0)
    {
        qDebug() << "[AudioSender] WSAStartup 失败:" << err;
        return false;
    }
    wsa_started_ = true;

    // ---- 第二步：创建 UDP socket ----
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET)
    {
        qDebug() << "[AudioSender] socket 创建失败:" << WSAGetLastError();
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    // ---- 第三步：设置目标地址 ----
    dest_addr_.sin_family = AF_INET;
    dest_addr_.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip, &dest_addr_.sin_addr);

    // ---- 第四步：设置发送缓冲区大小 ----
    int send_buf_size = 1 * 1024 * 1024;                   // 1MB（音频包小，不需要太大）
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               (const char*)&send_buf_size, sizeof(send_buf_size));

    qDebug() << "[AudioSender] 初始化成功 ->" << dest_ip << ":" << dest_port;
    return true;
}

// 关闭 socket 并清理 Winsock
void AudioSender::Close()
{
    if (sock_ != INVALID_SOCKET)
    {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (wsa_started_)
    {
        WSACleanup();
        wsa_started_ = false;
    }
}

// 将一帧 Opus 数据分片后通过 UDP 发送
void AudioSender::SendFrame(const uint8_t* frame_data, int frame_size,
                            uint16_t frame_index, uint32_t timestamp)
{
    if (sock_ == INVALID_SOCKET || !frame_data || frame_size <= 0)
        return;

    // ---- 复用 NalUnit.h 的 FragmentFrame 分片 ----
    // Opus 包通常 <400 字节，MAX_PAYLOAD_SIZE=1400 足够，基本单包送达
    auto packets = FragmentFrame(frame_data, frame_size,
                                 frame_index, timestamp, false);    // 音频没有关键帧概念

    // 逐片发送
    for (const auto& pkt : packets)
    {
        sendto(sock_, (const char*)pkt.data(), (int)pkt.size(), 0,
               (sockaddr*)&dest_addr_, sizeof(dest_addr_));
    }
}

AudioSender::AudioSender()
{
}

AudioSender::~AudioSender()
{
    Close();
}
