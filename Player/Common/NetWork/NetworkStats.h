#ifndef NETWORKSTATS_H
#define NETWORKSTATS_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// ========== 控制消息类型枚举 ==========

// 控制信道上的消息类型（在 CONTROL_MSG_MARKER=0xFF 后紧跟 1 字节类型）
// JSON 消息的 type 字段必须与此枚举值一致
enum class ControlMsgType : uint8_t {
    RequestIDR      = 0x01,   // 请求 IDR 帧（客户端→服务端）
    LossReport      = 0x02,   // 丢包统计上报（客户端→服务端）
    Ping            = 0x03,   // RTT 探测（客户端→服务端，payload=8字节timestamp）
    Pong            = 0x04,   // RTT 响应（服务端→客户端，payload=8字节timestamp原样回传）
    BitrateChange   = 0x05,   // 码率调整通知（服务端→客户端）
};

// ========== 网络统计数据结构 ==========

struct NetworkStats
{
    // 接收统计
    int receive_fps = 0;            // 接收帧率（每秒完整收到的帧数）
    int decode_fps = 0;             // 解码帧率（由解码器填充，VideoReceiver 不填）
    int render_fps = 0;             // 渲染帧率（由渲染器填充，VideoReceiver 不填）

    // 丢包统计
    float loss_rate = 0.0f;         // 丢包率（%，基于 UDP 包序号缺口）
    int lost_frames = 0;            // 丢失的帧数（每秒增量）

    // RTT
    int rtt_ms = 0;                 // 最新 RTT（毫秒），0=未测量

    // FEC 统计
    int fec_recovered = 0;          // FEC 恢复成功的帧数（每秒增量）
    int fec_failed = 0;             // FEC 恢复失败的帧数（每秒增量）

    // 带宽估算
    int estimated_bandwidth_kbps = 0; // 估算可用带宽（基于接收字节数/秒），0=未计算
};

// ========== JSON 序列化辅助函数 ==========

// 将 NetworkStats 序列化为 JSON 字符串（调用方负责释放内存）
inline std::string StatsToJson(const NetworkStats& stats)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        R"({"type":"loss_report","fps":%d,"loss_rate":%.1f,"rtt_ms":%d,"fec_rec":%d,"fec_fail":%d})",
        stats.receive_fps,
        stats.loss_rate,
        stats.rtt_ms,
        stats.fec_recovered,
        stats.fec_failed);
    return std::string(buf);
}

// 从 JSON 字符串解析 NetworkStats（简化解析，假设格式固定）
inline NetworkStats JsonToStats(const std::string& json)
{
    NetworkStats s;

    // 极简解析：找 "fps":数字, "loss_rate":数字, ...
    auto findInt = [&json](const char* key, int& out) {
        auto pos = json.find(key);
        if (pos != std::string::npos) {
            pos += strlen(key) + 1;  // skip key + ':'
            out = std::atoi(json.c_str() + pos);
        }
    };
    auto findFloat = [&json](const char* key, float& out) {
        auto pos = json.find(key);
        if (pos != std::string::npos) {
            pos += strlen(key) + 1;  // skip key + ':'
            out = static_cast<float>(std::atof(json.c_str() + pos));
        }
    };

    findInt("\"fps\":", s.receive_fps);
    findFloat("\"loss_rate\":", s.loss_rate);
    findInt("\"rtt_ms\":", s.rtt_ms);
    findInt("\"fec_rec\":", s.fec_recovered);
    findInt("\"fec_fail\":", s.fec_failed);

    return s;
}

#endif // NETWORKSTATS_H
