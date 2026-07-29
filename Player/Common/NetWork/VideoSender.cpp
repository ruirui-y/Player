#include "VideoSender.h"

#include <QDebug>

// 初始化 Winsock 并创建 UDP socket
bool VideoSender::Init(const char* dest_ip, uint16_t dest_port)
{
    // ---- 第一步：初始化 Winsock ----
    WSADATA wsa_data;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0)
    {
        qDebug() << "[VideoSender] WSAStartup 失败:" << err;
        return false;
    }
    wsa_started_ = true;

    // ---- 第二步：创建 UDP socket ----
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET)
    {
        qDebug() << "[VideoSender] socket 创建失败:" << WSAGetLastError();
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    // ---- 第三步：设置目标地址 ----
    dest_addr_.sin_family = AF_INET;
    dest_addr_.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip, &dest_addr_.sin_addr);

    // ---- 第四步：设置发送缓冲区大小（避免大帧丢包）----
    int send_buf_size = 4 * 1024 * 1024;                   // 4MB
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               (const char*)&send_buf_size, sizeof(send_buf_size));

    qDebug() << "[VideoSender] 初始化成功 ->" << dest_ip << ":" << dest_port;
    return true;
}

// 关闭 socket 并清理 Winsock
void VideoSender::Close()
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

// 将一帧 H.264 分片后通过 UDP 发送
void VideoSender::SendFrame(const uint8_t* frame_data, int frame_size,
                            uint16_t frame_index, uint32_t timestamp,
                            bool is_keyframe)
{
    if (sock_ == INVALID_SOCKET || !frame_data || frame_size <= 0)
        return;

    // ---- 第一步：分片 ----
    auto packets = FragmentFrame(frame_data, frame_size,
                                 frame_index, timestamp, is_keyframe);

    // ---- 第二步：逐片发送 ----
    for (const auto& pkt : packets)
    {
        sendto(sock_, (const char*)pkt.data(), (int)pkt.size(), 0,
               (sockaddr*)&dest_addr_, sizeof(dest_addr_));
    }
}

VideoSender::VideoSender()
{
}

VideoSender::~VideoSender()
{
    Close();
}
