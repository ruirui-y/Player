#ifndef BITRATECONTROLLER_H
#define BITRATECONTROLLER_H

#include <cstdint>
#include <chrono>

// 前向声明，避免循环依赖
struct NetworkStats;

// 码率自适应控制器
// 根据客户端上报的丢包率和 RTT，自动调整服务端编码码率
// 策略：丢包 >5% → ×0.85 快速降；丢包 <1% 且保持 2s → ×1.05 缓慢升
class BitrateController
{
public:
    // initial_kbps：初始码率
    // min_kbps：最低码率（不低于此值）
    // max_kbps：最高码率（不高于此值）
    BitrateController(int initial_kbps, int min_kbps = 5000, int max_kbps = 50000);

    // 收到客户端网络统计报告时调用，返回新的目标码率
    // 如果不需要调整，返回与当前相同的值
    int OnStatsReport(const NetworkStats& stats);

    // 获取当前目标码率
    int GetTargetBitrate() const { return target_bitrate_; }

private:
    int min_kbps_;                                              // 最低码率
    int max_kbps_;                                              // 最高码率
    int target_bitrate_;                                        // 当前目标码率
    int consecutive_low_loss_;                                  // 连续低丢包次数

    std::chrono::steady_clock::time_point last_adjust_time_;    // 上次调整时间
    std::chrono::steady_clock::time_point last_decrease_time_;  // 上次降码率时间

    static constexpr int ADJUST_INTERVAL_MS = 2000;             // 调整间隔（防抖）
    static constexpr float HIGH_LOSS_THRESHOLD = 5.0f;          // 高丢包阈值（%）
    static constexpr float MEDIUM_LOSS_THRESHOLD = 3.0f;        // 中丢包阈值（%）
    static constexpr float LOW_LOSS_THRESHOLD = 1.0f;           // 低丢包阈值（%）
    static constexpr float DECREASE_FACTOR = 0.85f;             // 降码率系数
    static constexpr float INCREASE_FACTOR = 1.05f;             // 升码率系数
    static constexpr int CONSECUTIVE_LOW_FOR_INCREASE = 3;      // 连续低丢包次数才升
};

#endif // BITRATECONTROLLER_H
