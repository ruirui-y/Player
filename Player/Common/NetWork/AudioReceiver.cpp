#include "AudioReceiver.h"
#include "Common/SafeQueue.h"
#include "Common/LogManager.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}

#include <QDebug>

// 初始化 Winsock 并绑定 UDP socket
bool AudioReceiver::Init(uint16_t listen_port, SafeQueue<AVPacket*>* packet_queue)
{
    if (!packet_queue)
    {
        qDebug() << "[AudioReceiver] packet_queue 为空";
        return false;
    }
    packet_queue_ = packet_queue;

    // ---- 第一步：初始化 Winsock ----
    WSADATA wsa_data;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0)
    {
        qDebug() << "[AudioReceiver] WSAStartup 失败:" << err;
        return false;
    }
    wsa_started_ = true;

    // ---- 第二步：创建 UDP socket ----
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET)
    {
        qDebug() << "[AudioReceiver] socket 创建失败:" << WSAGetLastError();
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    // ---- 第三步：设置接收缓冲区大小 ----
    int recv_buf_size = 1 * 1024 * 1024;                   // 1MB
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
               (const char*)&recv_buf_size, sizeof(recv_buf_size));

    // ---- 允许端口复用（客户端重启时旧 socket 可能未完全释放）----
    int reuse = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    // ---- 第四步：绑定监听端口 ----
    listen_addr_.sin_family = AF_INET;
    listen_addr_.sin_addr.s_addr = htonl(INADDR_ANY);      // 监听所有网卡
    listen_addr_.sin_port = htons(listen_port);

    if (bind(sock_, (sockaddr*)&listen_addr_, sizeof(listen_addr_)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        qDebug() << "[AudioReceiver] bind 失败:" << err;
        LogManager::Log("ERR", "[AudioReceiver] bind 失败, port=%d, WSAError=%d", listen_port, err);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    qDebug() << "[AudioReceiver] 绑定成功，监听端口:" << listen_port;
    return true;
}

// 启动接收线程
void AudioReceiver::Start()
{
    if (running_)
        return;

    running_ = true;
    recv_thread_ = std::thread(&AudioReceiver::ReceiveLoop, this);
    qDebug() << "[AudioReceiver] 接收线程已启动";
}

// 停止接收线程并关闭 socket
void AudioReceiver::Stop()
{
    if (!running_)
        return;

    running_ = false;

    // 关闭 socket 让 recvfrom 解除阻塞
    if (sock_ != INVALID_SOCKET)
    {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }

    if (recv_thread_.joinable())
        recv_thread_.join();

    qDebug() << "[AudioReceiver] 已停止，共收到" << total_packets_ << "个包，"
             << total_frames_ << "帧";
}

// ================================================================
// ---- 接收线程主循环：recvfrom → NalReassembler 组帧 → AVPacket 推入队列 ----
// ================================================================
void AudioReceiver::ReceiveLoop()
{
    // 接收缓冲区：最大 UDP 包 + 余量
    uint8_t recv_buf[2048];

    while (running_)
    {
        // ---- 第一步：阻塞接收 UDP 包 ----
        sockaddr_in from_addr{};
        int from_len = sizeof(from_addr);
        int recv_len = recvfrom(sock_, (char*)recv_buf, sizeof(recv_buf), 0,
                                 (sockaddr*)&from_addr, &from_len);
        if (recv_len <= 0)
        {
            // socket 关闭或出错，退出循环
            break;
        }

        ++total_packets_;

        // ---- 第二步：交给组帧器 ----
        bool frame_complete = reassembler_.AddPacket(recv_buf, recv_len);
        if (!frame_complete)
            continue;                                       // 还没收齐，继续等下一个包

        // ---- 第三步：取走完整帧 ----
        auto frame_data = reassembler_.TakeFrame();
        if (frame_data.empty())
            continue;

        ++total_frames_;

        // ---- 第四步：包装成 AVPacket 推入队列 ----
        AVPacket* pkt = av_packet_alloc();
        if (!pkt)
            continue;

        int ret = av_new_packet(pkt, static_cast<int>(frame_data.size()));
        if (ret < 0)
        {
            av_packet_free(&pkt);
            continue;
        }

        // 拷贝帧数据到 AVPacket
        memcpy(pkt->data, frame_data.data(), frame_data.size());

        // 设置 PTS（时间戳毫秒 → 1/48000 时基，与 Opus 解码器约定一致）
        // 48000 / 1000 = 48，所以毫秒值 × 48 = 48000 时基的 PTS
        pkt->pts = static_cast<int64_t>(reassembler_.GetTimestamp()) * 48;
        pkt->dts = pkt->pts;

        // 推入队列给 AudioDecoder 消费
        packet_queue_->Push(pkt);
    }

    qDebug() << "[AudioReceiver] 接收线程退出";
}

AudioReceiver::AudioReceiver()
{
}

AudioReceiver::~AudioReceiver()
{
    Stop();

    if (wsa_started_)
    {
        WSACleanup();
        wsa_started_ = false;
    }
}
