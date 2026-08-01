#include "RttMeasurer.h"
#include <QDebug>

RttMeasurer::RttMeasurer()
{
}

RttMeasurer::~RttMeasurer()
{
    Stop();
}

void RttMeasurer::SetSendCallback(std::function<void(uint64_t timestamp_ms)> callback)
{
    send_callback_ = std::move(callback);
}

void RttMeasurer::OnPongReceived(uint64_t sent_timestamp_ms)
{
    // 计算 RTT：当前时间 - 发送时间
    auto now = std::chrono::steady_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    int rtt = static_cast<int>(now_ms - static_cast<int64_t>(sent_timestamp_ms));
    if (rtt < 0) rtt = 0;                                   // 时钟回拨保护
    if (rtt > 5000) rtt = 5000;                             // 上限 5 秒（异常保护）

    latest_rtt_ms_.store(rtt);
}

void RttMeasurer::Start()
{
    if (running_)
        return;

    running_ = true;
    ping_thread_ = std::thread(&RttMeasurer::PingLoop, this);
}

void RttMeasurer::Stop()
{
    if (!running_)
        return;

    running_ = false;
    if (ping_thread_.joinable())
        ping_thread_.join();

    qDebug() << "[RttMeasurer] 已停止";
}

void RttMeasurer::PingLoop()
{
    qDebug() << "[RttMeasurer] Ping 循环启动, 间隔=" << PING_INTERVAL_MS << "ms";

    while (running_)
    {
        if (send_callback_)
        {
            auto now = std::chrono::steady_clock::now();
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();

            send_callback_(static_cast<uint64_t>(now_ms));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(PING_INTERVAL_MS));
    }

    qDebug() << "[RttMeasurer] Ping 循环退出";
}
