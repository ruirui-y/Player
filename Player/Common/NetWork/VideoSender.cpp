#include "VideoSender.h"

#include <QDebug>

// ---- FEC 编码：为同一组数据包生成 XOR 校验包 ----
// data_packets：组内数据包（全部载荷，含包头）
// frame_index、timestamp：填入校验包
// fec_group_id：FEC 组 ID
// fec_data_count：组内数据包数量
// 返回：一个校验包（可直接 sendto）
static std::vector<uint8_t> GenerateFECParity(
    const std::vector<std::vector<uint8_t>>& data_packets,
    uint16_t frame_index,
    uint32_t timestamp,
    uint8_t fec_group_id,
    int fec_data_count)
{
    // 取组内最大负载大小（各包大小可能不同）
    size_t max_payload = 0;
    for (const auto& pkt : data_packets)
        max_payload = std::max(max_payload, pkt.size() - NAL_HEADER_SIZE);

    // 校验包 = 包头（11 字节）+ 负载（与最大数据包负载等长）
    std::vector<uint8_t> parity(NAL_HEADER_SIZE + max_payload, 0);

    // 填充包头
    NalPacketHeader hdr;
    hdr.flags = FLAG_FEC_PARITY;
    hdr.frame_index = frame_index;
    hdr.packet_index = static_cast<uint16_t>(fec_data_count);  // 紧随数据包后
    hdr.timestamp = timestamp;
    hdr.fec_group_id = fec_group_id;
    hdr.fec_data_count = static_cast<uint8_t>(fec_data_count);
    std::memcpy(parity.data(), &hdr, NAL_HEADER_SIZE);

    // XOR 所有数据包的负载部分
    auto parity_payload = parity.data() + NAL_HEADER_SIZE;
    for (const auto& pkt : data_packets)
    {
        auto data_payload = pkt.data() + NAL_HEADER_SIZE;
        auto data_len = pkt.size() - NAL_HEADER_SIZE;

        for (size_t i = 0; i < data_len; ++i)
            parity_payload[i] ^= data_payload[i];
        // 短包尾部等价于 XOR 0，不需要额外处理
    }

    return parity;
}

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

// 将一帧 H.264 分片后通过 UDP 发送（含 FEC 校验包）
void VideoSender::SendFrame(const uint8_t* frame_data, int frame_size,
                            uint16_t frame_index, uint32_t timestamp,
                            bool is_keyframe)
{
    if (sock_ == INVALID_SOCKET || !frame_data || frame_size <= 0)
        return;

    // ---- 第一步：分片 ----
    auto packets = FragmentFrame(frame_data, frame_size,
                                 frame_index, timestamp, is_keyframe);

    int total_data_packets = static_cast<int>(packets.size());

    // ---- 第二步：按 FEC_DATA_SHARDS 分组，每组插入一个校验包 ----
    uint8_t group_id = 0;
    std::vector<std::vector<uint8_t>> group_buffer;
    group_buffer.reserve(FEC_DATA_SHARDS);

    for (int i = 0; i < total_data_packets; ++i)
    {
        // 填充 fec_group_id 到数据包
        NalPacketHeader* hdr = reinterpret_cast<NalPacketHeader*>(packets[i].data());
        hdr->fec_group_id = group_id;

        group_buffer.push_back(std::move(packets[i]));

        if (static_cast<int>(group_buffer.size()) >= FEC_DATA_SHARDS ||
            i == total_data_packets - 1)
        {
            int data_count = static_cast<int>(group_buffer.size());

            // 发送组内所有数据包
            for (const auto& dp : group_buffer)
            {
                sendto(sock_, (const char*)dp.data(), (int)dp.size(), 0,
                       (sockaddr*)&dest_addr_, sizeof(dest_addr_));
            }

            // 生成并发送校验包
            auto parity = GenerateFECParity(group_buffer, frame_index,
                                            timestamp, group_id, data_count);
            sendto(sock_, (const char*)parity.data(), (int)parity.size(), 0,
                   (sockaddr*)&dest_addr_, sizeof(dest_addr_));

            // 下一组
            group_buffer.clear();
            group_id++;
        }
    }
}

VideoSender::VideoSender()
{
}

VideoSender::~VideoSender()
{
    Close();
}
