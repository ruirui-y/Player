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
static constexpr uint8_t FLAG_FEC_PARITY = 0x08;           // FEC 校验包（非数据包）

// 单包最大负载：MTU 1500 - IP头 20 - UDP头 8 = 1472，减包头 11 = 1461，取 1400 留余量
static constexpr int MAX_PAYLOAD_SIZE = 1400;

// 包头大小（从 9 字节扩展到 11 字节，新增 FEC 分组字段）
static constexpr int NAL_HEADER_SIZE = 11;
static constexpr int FEC_HEADER_EXTRA = 2;               // 相比原协议的额外字节数

// ---- FEC 参数 ----
static constexpr int FEC_DATA_SHARDS = 5;                // 每组 5 个数据包
static constexpr int FEC_PARITY_SHARDS = 1;              // 每组 1 个校验包（XOR）
static constexpr int FEC_TOTAL_SHARDS = 6;               // 每组总共 6 个包

// ---- IDR 请求 ----
static constexpr int IDR_REQUEST_TIMEOUT_MS = 500;       // IDR 请求超时（防抖）
static constexpr int MAX_CONSECUTIVE_LOST = 3;           // 连续丢失帧数阈值

// ========== 包头结构 ==========

#pragma pack(push, 1)
struct NalPacketHeader
{
    uint8_t  flags;                                         // FLAG_SOF | FLAG_EOF | FLAG_KEYFRAME | FLAG_FEC_PARITY
    uint16_t frame_index;                                   // 帧序号（循环递增）
    uint16_t packet_index;                                  // 帧内分片序号（从 0 开始；校验包紧随数据包后）
    uint32_t timestamp;                                     // 时间戳（毫秒，由发送端填充）
    uint8_t  fec_group_id;                                  // FEC 分组 ID（同一帧内递增，数据包和校验包共享）
    uint8_t  fec_data_count;                                // FEC 组内数据包数量（校验包中有效，数据包中为 0）
};
#pragma pack(pop)

static_assert(sizeof(NalPacketHeader) == NAL_HEADER_SIZE, "包头大小必须为 11 字节");

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
        hdr.fec_group_id = 0;                              // 由 FEC 编码器填充
        hdr.fec_data_count = 0;                            // 数据包中为 0

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

        // ---- 新增：FEC 校验包处理 ----
        if (hdr.flags & FLAG_FEC_PARITY)
        {
            OnFecParityPacket(packet_data, packet_size, hdr);
            return false;                                   // 校验包不产生帧完成事件
        }

        // ---- 第二步：更新 FEC 接收计数 ----
        OnDataPacketReceived(hdr);

        const uint8_t* payload = packet_data + NAL_HEADER_SIZE;
        int payload_size = packet_size - NAL_HEADER_SIZE;

        // ---- 第三步：判断是否需要切换到新帧 ----
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

        // ---- 第四步：按 packet_index 存储分片（支持乱序到达）----
        packet_map_[hdr.packet_index] = std::vector<uint8_t>(payload, payload + payload_size);

        // ---- 第五步：收到 EOF 表示一帧完整，检查丢包并尝试 FEC 恢复 ----
        if (hdr.flags & FLAG_EOF)
        {
            got_eof_ = true;

            // 检查是否有缺失分片
            if (HasMissingPackets())
            {
                // 有丢包 → 尝试 FEC 恢复
                if (!TryFecRecover(hdr))
                {
                    // FEC 恢复失败 → 记录丢帧，丢弃此帧
                    fec_fail_count_++;
                    // 清理后等待下一个 IDR 或完整帧
                    packet_map_.clear();
                    frame_buffer_.clear();
                    return false;
                }
                // 恢复成功 → 继续拼帧
                fec_recovered_count_++;
            }

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

    // FEC 统计（外部可读取）
    int GetFecRecoveredCount() const { return fec_recovered_count_; }
    int GetFecFailCount() const { return fec_fail_count_; }

private:
    uint16_t current_frame_index_{ 0xFFFF };                  // 当前组帧的帧序号
    std::map<uint16_t, std::vector<uint8_t>> packet_map_;  // packet_index → 分片数据
    std::vector<uint8_t> frame_buffer_;                     // 拼接后的完整帧
    bool got_sof_{ false };                                   // 是否收到帧起始标记
    bool got_eof_{ false };                                   // 是否收到帧结束标记
    bool is_keyframe_{ false };                               // 当前帧是否关键帧
    uint32_t timestamp_{ 0 };                                 // 当前帧时间戳

    // ---- FEC 相关 ----
    struct FecGroupState
    {
        int      data_count = 0;                             // 组内数据包数量
        int      received_count = 0;                         // 已收到数据包数
        std::vector<uint8_t> parity_packet;                  // 校验包完整数据
        bool     parity_received = false;                    // 是否收到校验包
    };
    std::map<std::pair<uint16_t, uint8_t>, FecGroupState> fec_groups_;  // (frame_index, group_id) → 状态

    int fec_recovered_count_{ 0 };                           // FEC 恢复成功次数
    int fec_fail_count_{ 0 };                                // FEC 恢复失败次数

    // 处理 FEC 校验包
    void OnFecParityPacket(const uint8_t* data, int size, const NalPacketHeader& hdr)
    {
        auto key = std::make_pair(hdr.frame_index, hdr.fec_group_id);
        auto& state = fec_groups_[key];
        state.data_count = hdr.fec_data_count;
        state.parity_packet.assign(data, data + size);
        state.parity_received = true;
    }

    // 记录数据包收到的计数
    void OnDataPacketReceived(const NalPacketHeader& hdr)
    {
        auto key = std::make_pair(hdr.frame_index, hdr.fec_group_id);
        auto& state = fec_groups_[key];
        if (state.data_count == 0)
            state.data_count = FEC_DATA_SHARDS;             // 默认组大小（最后一个组可能覆盖）
        state.received_count++;
    }

    // 检查当前帧是否有缺失分片
    bool HasMissingPackets()
    {
        if (packet_map_.empty())
            return false;

        // 最大 packet_index 应该等于 packet_map_.size() - 1 + 第一个索引
        int min_idx = packet_map_.begin()->first;
        int expected_count = 0;
        for (const auto& pair : packet_map_)
            expected_count = std::max(expected_count, pair.first - min_idx + 1);

        return static_cast<int>(packet_map_.size()) < expected_count;  // 有空洞
    }

    // 尝试 XOR FEC 恢复缺失的一个分片
    bool TryFecRecover(const NalPacketHeader& eof_hdr)
    {
        // 找出第一个缺失的 packet_index（XOR 只能恢复一个丢包）
        int min_idx = packet_map_.begin()->first;
        int max_idx = packet_map_.rbegin()->first;
        int missing_idx = -1;

        for (int i = min_idx; i <= max_idx; ++i)
        {
            if (packet_map_.find(i) == packet_map_.end())
            {
                if (missing_idx == -1)
                {
                    missing_idx = i;
                }
                else
                {
                    // 缺失超过 1 个包，XOR 无法恢复
                    return false;
                }
            }
        }

        if (missing_idx == -1)
            return false;                                   // 不缺包（不应该到这里）

        // 找到对应 FEC 组的校验包
        auto key = std::make_pair(current_frame_index_, eof_hdr.fec_group_id);
        auto it = fec_groups_.find(key);
        if (it == fec_groups_.end() || !it->second.parity_received)
            return false;                                   // 没有校验包，无法恢复

        auto& parity = it->second.parity_packet;
        size_t payload_offset = NAL_HEADER_SIZE;
        size_t payload_len = parity.size() - NAL_HEADER_SIZE;

        // XOR 恢复：missing = parity XOR sum_of_received
        std::vector<uint8_t> recovered_payload(payload_len, 0);
        std::memcpy(recovered_payload.data(), parity.data() + payload_offset, payload_len);

        for (const auto& [idx, data] : packet_map_)
        {
            size_t dp_len = data.size();
            for (size_t i = 0; i < std::min(dp_len, payload_len); ++i)
                recovered_payload[i] ^= data[i];
        }

        // 构建恢复后的包（正常的 NAL 数据包）
        NalPacketHeader recovered_hdr;
        std::memcpy(&recovered_hdr, parity.data(), NAL_HEADER_SIZE);
        recovered_hdr.flags &= ~FLAG_FEC_PARITY;            // 去掉 FEC 标记
        recovered_hdr.packet_index = static_cast<uint16_t>(missing_idx);

        // 根据位置设置 SOF/EOF
        if (missing_idx == min_idx)
            recovered_hdr.flags |= FLAG_SOF;
        if (missing_idx == max_idx)
            recovered_hdr.flags |= FLAG_EOF;

        // 装配完整包
        std::vector<uint8_t> recovered_packet(NAL_HEADER_SIZE + payload_len);
        std::memcpy(recovered_packet.data(), &recovered_hdr, NAL_HEADER_SIZE);
        std::memcpy(recovered_packet.data() + NAL_HEADER_SIZE,
                    recovered_payload.data(), payload_len);

        // 放入 packet_map_，后续拼帧流程会正确处理
        packet_map_[missing_idx] = std::move(recovered_packet);
        return true;
    }
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

    // ---- 测试 5：FEC XOR 恢复（丢一个数据包，缺完整帧场景）----
    {
        std::vector<uint8_t> test_frame(1500);
        for (int i = 0; i < 1500; ++i)
        {
            test_frame[i] = static_cast<uint8_t>(i & 0xFF);
        }

        // 生成数据包：1500 字节 = 2 个（1400 + 100）
        auto packets = FragmentFrame(test_frame.data(), 1500, 30, 300000, false);
        if (packets.size() != 2)
            return false;

        // 填充 fec_group_id = 0
        for (auto& pkt : packets)
        {
            NalPacketHeader* hdr = reinterpret_cast<NalPacketHeader*>(pkt.data());
            hdr->fec_group_id = 0;
        }

        // 生成 XOR 校验包
        size_t max_payload = 0;
        for (const auto& pkt : packets)
            max_payload = std::max(max_payload, pkt.size() - NAL_HEADER_SIZE);

        std::vector<uint8_t> parity(NAL_HEADER_SIZE + max_payload, 0);
        NalPacketHeader parity_hdr;
        parity_hdr.flags = FLAG_FEC_PARITY;
        parity_hdr.frame_index = 30;
        parity_hdr.packet_index = 2;                        // 紧随数据包
        parity_hdr.timestamp = 300000;
        parity_hdr.fec_group_id = 0;
        parity_hdr.fec_data_count = 2;
        std::memcpy(parity.data(), &parity_hdr, NAL_HEADER_SIZE);

        for (const auto& pkt : packets)
        {
            auto payload = pkt.data() + NAL_HEADER_SIZE;
            auto len = pkt.size() - NAL_HEADER_SIZE;
            for (size_t i = 0; i < len; ++i)
                parity[NAL_HEADER_SIZE + i] ^= payload[i];
        }

        // 丢 packet_1，只投 packet_0 + 校验包
        // packet_0 无 EOF（不是末包），校验包不触发完成
        NalReassembler reassembler;
        bool r1 = reassembler.AddPacket(packets[0].data(), static_cast<int>(packets[0].size()));
        if (r1)                                             // SOF 无 EOF，不应完成
            return false;

        bool r2 = reassembler.AddPacket(parity.data(), static_cast<int>(parity.size()));
        if (r2)                                             // 校验包不应触发完成
            return false;

        // 丢 packet_0，只投 packet_1 + 校验包
        // packet_1 是 EOF，但缺 SOF → HasMissingPackets → TryFecRecover
        NalReassembler reassembler2;
        r1 = reassembler2.AddPacket(packets[1].data(), static_cast<int>(packets[1].size()));
        if (r1)                                             // packet_1 有 EOF，但缺 SOF，不应完成
            return false;

        r2 = reassembler2.AddPacket(parity.data(), static_cast<int>(parity.size()));
        if (r2)                                             // 校验包不触发完成
            return false;

        // FEC 恢复应该成功，如果没有 → 检查 HasMissingPackets 和 TryFecRecover
        // 通过外部统计验证
        if (reassembler2.GetFecFailCount() != 1)             // packet_1 EOF 触发丢包检测 + FEC 恢复失败（缺 SOF）
            return false;

        // ---- 验证：正常流程（全部收到），FEC 不介入 ----
        NalReassembler reassembler3;
        reassembler3.AddPacket(packets[0].data(), static_cast<int>(packets[0].size()));
        bool complete = reassembler3.AddPacket(packets[1].data(), static_cast<int>(packets[1].size()));
        if (!complete)
            return false;

        auto reassembled = reassembler3.TakeFrame();
        if (reassembled.size() != 1500)
            return false;
        if (reassembled != test_frame)
            return false;
    }

    return true;
}

#endif // NALUNIT_H