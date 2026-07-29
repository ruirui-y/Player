#ifndef NALUNIT_H
#define NALUNIT_H

#include <cstdint>                                          // 固定宽度整型
#include <vector>                                           // 动态数组
#include <cstring>                                          // memcpy
#include <algorithm>                                        // std::min
#include <map>                                              // 按 packet_index 存储分片

// ========== 常量定义 ==========

// 分片标记位（flags 字段的 bit 定义）
static constexpr uint8_t FLAG_SOF = 0x01;             // 帧起始分片
static constexpr uint8_t FLAG_EOF = 0x02;             // 帧结束分片
static constexpr uint8_t FLAG_KEYFRAME = 0x04;             // 关键帧（IDR）

// 单包最大负载：MTU 1500 - IP头 20 - UDP头 8 = 1472，减包头 9 = 1463，取 1400 留余量
static constexpr int MAX_PAYLOAD_SIZE = 1400;

// 包头大小
static constexpr int NAL_HEADER_SIZE = 9;

// ========== 包头结构 ==========

#pragma pack(push, 1)
struct NalPacketHeader
{
    uint8_t  flags;                                         // FLAG_SOF | FLAG_EOF | FLAG_KEYFRAME
    uint16_t frame_index;                                   // 帧序号（循环递增）
    uint16_t packet_index;                                  // 帧内分片序号（从 0 开始）
    uint32_t timestamp;                                     // 时间戳（毫秒，由发送端填充）
};
#pragma pack(pop)

static_assert(sizeof(NalPacketHeader) == NAL_HEADER_SIZE, "包头大小必须为 9 字节");

// ========== 分片器：将一帧 H.264 数据拆分为 UDP 包 ==========

// 将一帧 H.264 Annex B 数据分片为多个 UDP 包
// frame_data：帧数据指针，frame_size：帧数据大小
// frame_index：帧序号，timestamp：时间戳，is_keyframe：是否关键帧
// 返回：每个元素是一个完整的 UDP 包（含包头 + 负载），可直接 sendto
inline std::vector<std::vector<uint8_t>> FragmentFrame(
    const uint8_t* frame_data,
    int frame_size,
    uint16_t frame_index,
    uint32_t timestamp,
    bool is_keyframe)
{
    std::vector<std::vector<uint8_t>> packets;

    // ---- 第一步：计算分片数量 ----
    int packet_count = (frame_size + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
    if (packet_count == 0)
    {
        packet_count = 1;                                   // 空帧也发一个包，保证 SOF+EOF
    }

    // ---- 第二步：逐片填充包头和负载 ----
    for (int i = 0; i < packet_count; ++i)
    {
        // 计算当前分片的负载偏移和大小
        int offset = i * MAX_PAYLOAD_SIZE;
        int payload_size = std::min(MAX_PAYLOAD_SIZE, frame_size - offset);

        // 构建包头
        NalPacketHeader hdr;
        hdr.flags = 0;
        hdr.frame_index = frame_index;
        hdr.packet_index = static_cast<uint16_t>(i);
        hdr.timestamp = timestamp;

        // 首片设 SOF
        if (i == 0)
        {
            hdr.flags |= FLAG_SOF;
        }

        // 末片设 EOF
        if (i == packet_count - 1)
        {
            hdr.flags |= FLAG_EOF;
        }

        // 关键帧所有分片都带 KEYFRAME 标记
        if (is_keyframe)
        {
            hdr.flags |= FLAG_KEYFRAME;
        }

        // ---- 第三步：组装完整的 UDP 包（包头 + 负载）----
        std::vector<uint8_t> packet(NAL_HEADER_SIZE + payload_size);
        std::memcpy(packet.data(), &hdr, NAL_HEADER_SIZE);
        std::memcpy(packet.data() + NAL_HEADER_SIZE, frame_data + offset, payload_size);

        packets.push_back(std::move(packet));
    }

    return packets;
}

// ========== 组帧器：将 UDP 包重组为完整帧 ==========

class NalReassembler
{
public:
    // 添加一个收到的 UDP 包，返回 true 表示一帧已完整
    bool AddPacket(const uint8_t* packet_data, int packet_size)
    {
        // ---- 第一步：校验包大小 ----
        if (packet_size < NAL_HEADER_SIZE)
        {
            return false;                                   // 包太小，丢弃
        }

        // 解析包头
        NalPacketHeader hdr;
        std::memcpy(&hdr, packet_data, NAL_HEADER_SIZE);

        const uint8_t* payload = packet_data + NAL_HEADER_SIZE;
        int payload_size = packet_size - NAL_HEADER_SIZE;

        // ---- 第二步：判断是否需要切换到新帧 ----
        // 只有帧序号不同或上一帧已完成时才切换，SOF 到达同一帧不重置（支持乱序）
        bool new_frame = false;
        if (hdr.frame_index != current_frame_index_)
        {
            new_frame = true;                               // 不同帧号 = 新帧（可能丢包了）
        }
        else if ((hdr.flags & FLAG_SOF) && got_eof_)
        {
            new_frame = true;                               // 同帧号但上一帧已完成 = 新一轮
        }
        else if (got_eof_)
        {
            // 同一帧已完成，这是重复包，丢弃
            return false;
        }

        if (new_frame)
        {
            // 上一帧如果不完整，数据被覆盖丢弃
            current_frame_index_ = hdr.frame_index;
            packet_map_.clear();                            // 清空分片映射表
            frame_buffer_.clear();
            got_sof_ = (hdr.flags & FLAG_SOF) != 0;
            got_eof_ = false;
            is_keyframe_ = (hdr.flags & FLAG_KEYFRAME) != 0;
            timestamp_ = hdr.timestamp;
        }

        // ---- 第三步：按 packet_index 存储分片（支持乱序到达）----
        packet_map_[hdr.packet_index] = std::vector<uint8_t>(payload, payload + payload_size);

        // ---- 第四步：收到 EOF 表示一帧完整，按序拼接 ----
        if (hdr.flags & FLAG_EOF)
        {
            got_eof_ = true;

            // 按 packet_index 升序拼接所有分片
            frame_buffer_.clear();
            for (const auto& pair : packet_map_)
            {
                frame_buffer_.insert(frame_buffer_.end(), pair.second.begin(), pair.second.end());
            }

            return true;                                    // 帧完整，调用方可取走
        }

        return false;
    }

    // 取走重组后的帧数据（调用后内部缓冲清空，准备下一帧）
    std::vector<uint8_t> TakeFrame()
    {
        auto result = std::move(frame_buffer_);
        frame_buffer_.clear();
        packet_map_.clear();
        got_sof_ = false;
        got_eof_ = false;
        return result;
    }

    uint16_t GetFrameIndex() const { return current_frame_index_; }   // 当前帧序号
    uint32_t GetTimestamp() const { return timestamp_; }              // 当前帧时间戳
    bool IsKeyFrame() const { return is_keyframe_; }                 // 当前帧是否关键帧

private:
    uint16_t current_frame_index_{ 0xFFFF };                  // 当前组帧的帧序号
    std::map<uint16_t, std::vector<uint8_t>> packet_map_;  // packet_index → 分片数据
    std::vector<uint8_t> frame_buffer_;                     // 拼接后的完整帧
    bool got_sof_{ false };                                   // 是否收到帧起始标记
    bool got_eof_{ false };                                   // 是否收到帧结束标记
    bool is_keyframe_{ false };                               // 当前帧是否关键帧
    uint32_t timestamp_{ 0 };                                 // 当前帧时间戳
};

// ========== 自测函数：验证分片/组帧正确性 ==========

// 返回 true 表示所有测试通过，可在 main 或测试函数中调用
inline bool NalUnitSelfTest()
{
    // ---- 测试 1：多分片帧的拆分和重组 ----
    {
        // 构造 3000 字节的测试帧（需要 3 个分片：1400 + 1400 + 200）
        std::vector<uint8_t> test_frame(3000);
        for (int i = 0; i < 3000; ++i)
        {
            test_frame[i] = static_cast<uint8_t>(i & 0xFF);
        }

        // 分片
        auto packets = FragmentFrame(test_frame.data(), 3000, 1, 90000, true);
        if (packets.size() != 3)                            // 应该有 3 个分片
        {
            return false;
        }

        // 检查首片标志：有 SOF，无 EOF
        NalPacketHeader hdr0;
        std::memcpy(&hdr0, packets[0].data(), NAL_HEADER_SIZE);
        if (!(hdr0.flags & FLAG_SOF) || (hdr0.flags & FLAG_EOF))
        {
            return false;
        }

        // 检查中间片标志：无 SOF，无 EOF
        NalPacketHeader hdr1;
        std::memcpy(&hdr1, packets[1].data(), NAL_HEADER_SIZE);
        if ((hdr1.flags & FLAG_SOF) || (hdr1.flags & FLAG_EOF))
        {
            return false;
        }

        // 检查末片标志：无 SOF，有 EOF
        NalPacketHeader hdr2;
        std::memcpy(&hdr2, packets[2].data(), NAL_HEADER_SIZE);
        if ((hdr2.flags & FLAG_SOF) || !(hdr2.flags & FLAG_EOF))
        {
            return false;
        }

        // 重组
        NalReassembler reassembler;
        bool complete = false;
        for (const auto& pkt : packets)
        {
            complete = reassembler.AddPacket(pkt.data(), static_cast<int>(pkt.size()));
        }
        if (!complete)                                      // 重组应该完成
        {
            return false;
        }

        auto reassembled = reassembler.TakeFrame();
        if (reassembled.size() != 3000)                     // 重组大小应该一致
        {
            return false;
        }
        if (reassembled != test_frame)                      // 重组数据应该完全匹配
        {
            return false;
        }
        if (!reassembler.IsKeyFrame())                      // 关键帧标记应该保留
        {
            return false;
        }
    }

    // ---- 测试 2：单分片帧（SOF + EOF 同时设置在同一包）----
    {
        std::vector<uint8_t> small_frame(100, 0xAA);

        auto packets = FragmentFrame(small_frame.data(), 100, 5, 180000, false);
        if (packets.size() != 1)                            // 小帧应该只有 1 个分片
        {
            return false;
        }

        // 单分片应该同时有 SOF 和 EOF
        NalPacketHeader hdr;
        std::memcpy(&hdr, packets[0].data(), NAL_HEADER_SIZE);
        if (!(hdr.flags & FLAG_SOF) || !(hdr.flags & FLAG_EOF))
        {
            return false;
        }

        // 单分片应该 AddPacket 后立即返回 true
        NalReassembler reassembler;
        bool complete = reassembler.AddPacket(packets[0].data(), static_cast<int>(packets[0].size()));
        if (!complete)
        {
            return false;
        }

        auto reassembled = reassembler.TakeFrame();
        if (reassembled != small_frame)                     // 数据应该完全匹配
        {
            return false;
        }
        if (reassembler.IsKeyFrame())                       // 不应该是关键帧
        {
            return false;
        }
    }

    // ---- 测试 3：帧切换（旧帧未完成时收到新帧 SOF）----
    {
        std::vector<uint8_t> frame_a(2000, 0x11);
        std::vector<uint8_t> frame_b(500, 0x22);

        auto packets_a = FragmentFrame(frame_a.data(), 2000, 1, 90000, true);
        auto packets_b = FragmentFrame(frame_b.data(), 500, 2, 93633, false);

        NalReassembler reassembler;

        // 只发 frame_a 的第一个包，帧不完整
        reassembler.AddPacket(packets_a[0].data(), static_cast<int>(packets_a[0].size()));

        // 收到 frame_b 的 SOF，应触发帧切换，丢弃 frame_a 残留数据
        bool complete = reassembler.AddPacket(packets_b[0].data(), static_cast<int>(packets_b[0].size()));
        if (!complete)                                      // frame_b 是单分片，应立即完成
        {
            return false;
        }

        auto reassembled = reassembler.TakeFrame();
        if (reassembled != frame_b)                         // 应该得到 frame_b 的数据
        {
            return false;
        }
    }

    // ---- 测试 4：乱序包重组（中间包先到）----
    {
        std::vector<uint8_t> test_frame(3500);
        for (int i = 0; i < 3500; ++i)
        {
            test_frame[i] = static_cast<uint8_t>((i * 7) & 0xFF);
        }

        auto packets = FragmentFrame(test_frame.data(), 3500, 10, 100000, false);
        // 3500 字节 = 3 个分片：1400 + 1400 + 700
        // packets[0]: SOF, packets[1]: middle, packets[2]: EOF

        NalReassembler reassembler;

        // 按 1, 0, 2 的顺序投递（中间包先到）
        reassembler.AddPacket(packets[1].data(), static_cast<int>(packets[1].size()));
        reassembler.AddPacket(packets[0].data(), static_cast<int>(packets[0].size()));
        bool complete = reassembler.AddPacket(packets[2].data(), static_cast<int>(packets[2].size()));

        if (!complete)
        {
            return false;
        }

        auto reassembled = reassembler.TakeFrame();
        if (reassembled != test_frame)                      // 乱序不应该影响数据正确性
        {
            return false;
        }
    }

    return true;
}

#endif // NALUNIT_H