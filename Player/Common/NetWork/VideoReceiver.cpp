#include "VideoReceiver.h"
#include "Common/SafeQueue.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}

#include <QDebug>

// 初始化 Winsock 并绑定 UDP socket
bool VideoReceiver::Init(uint16_t listen_port, SafeQueue<AVPacket*>* packet_queue)
{
    if (!packet_queue)
    {
        qDebug() << "[VideoReceiver] packet_queue 为空";
        return false;
    }
    packet_queue_ = packet_queue;

    // ---- 第一步：初始化 Winsock ----
    WSADATA wsa_data;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0)
    {
        qDebug() << "[VideoReceiver] WSAStartup 失败:" << err;
        return false;
    }
    wsa_started_ = true;

    // ---- 第二步：创建 UDP socket ----
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET)
    {
        qDebug() << "[VideoReceiver] socket 创建失败:" << WSAGetLastError();
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    // ---- 第三步：设置接收缓冲区大小 ----
    int recv_buf_size = 4 * 1024 * 1024;                   // 4MB
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
               (const char*)&recv_buf_size, sizeof(recv_buf_size));

    // ---- 第四步：绑定监听端口 ----
    listen_addr_.sin_family = AF_INET;
    listen_addr_.sin_addr.s_addr = htonl(INADDR_ANY);      // 监听所有网卡
    listen_addr_.sin_port = htons(listen_port);

    if (bind(sock_, (sockaddr*)&listen_addr_, sizeof(listen_addr_)) == SOCKET_ERROR)
    {
        qDebug() << "[VideoReceiver] bind 失败:" << WSAGetLastError();
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    qDebug() << "[VideoReceiver] 绑定成功，监听端口:" << listen_port;
    return true;
}

// 启动接收线程
void VideoReceiver::Start()
{
    if (running_)
        return;

    running_ = true;
    recv_thread_ = std::thread(&VideoReceiver::ReceiveLoop, this);
    qDebug() << "[VideoReceiver] 接收线程已启动";
}

// 停止接收线程并关闭 socket
void VideoReceiver::Stop()
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

    qDebug() << "[VideoReceiver] 已停止，共收到" << total_packets_ << "个包，"
             << total_frames_ << "帧";
}

// 接收线程主循环
void VideoReceiver::ReceiveLoop()
{
    // 接收缓冲区：最大 UDP 包 + 余量
    uint8_t recv_buf[2048];

    while (running_)
    {
        // ---- 第一步：阻塞接收 UDP 包 ----
        int recv_len = recvfrom(sock_, (char*)recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
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
        // av_new_packet 分配的 buffer 会自动释放，下游 av_packet_free 时回收
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

        // 关键帧标记
        if (reassembler_.IsKeyFrame())
        {
            pkt->flags |= AV_PKT_FLAG_KEY;
        }

        // 设置 PTS（用时间戳，单位毫秒 → 转为 1/90000 时基与 H.264 约定一致）
        pkt->pts = static_cast<int64_t>(reassembler_.GetTimestamp()) * 90;
        pkt->dts = pkt->pts;

        // 推入队列给 VideoDecoder 消费
        packet_queue_->Push(pkt);
    }

    qDebug() << "[VideoReceiver] 接收线程退出";
}

VideoReceiver::VideoReceiver()
{
}

VideoReceiver::~VideoReceiver()
{
    Stop();

    if (wsa_started_)
    {
        WSACleanup();
        wsa_started_ = false;
    }
}
