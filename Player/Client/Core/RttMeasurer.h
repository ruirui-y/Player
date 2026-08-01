#ifndef RTTMEASURER_H
#define RTTMEASURER_H

#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <chrono>

// RTT 测量器
// 启动一个线程，每 2 秒发 Ping（通过回调发送时间戳）
// 收到 Pong 时计算 RTT = 当前时间 - 发送时间
// 外部通过 GetLatestRtt() 读取最新值
class RttMeasurer
{
public:
    RttMeasurer();
    ~RttMeasurer();

    // 设置发送 Ping 的回调（通过控制信道发送 8 字节时间戳）
    void SetSendCallback(std::function<void(uint64_t timestamp_ms)> callback);

    // 收到 Pong 时调用（由控制信道接收线程触发）
    // sent_timestamp_ms：原样回传的发送时间戳
    void OnPongReceived(uint64_t sent_timestamp_ms);

    // 获取最新 RTT（毫秒），0=未测量
    int GetLatestRtt() const { return latest_rtt_ms_.load(); }

    // 开始周期性 Ping（2 秒一次）
    void Start();

    // 停止 Ping
    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    void PingLoop();

    std::function<void(uint64_t)> send_callback_;
    std::atomic<bool> running_{false};
    std::thread ping_thread_;
    std::atomic<int> latest_rtt_ms_{0};

    static constexpr int PING_INTERVAL_MS = 2000;           // Ping 间隔
};

#endif // RTTMEASURER_H
