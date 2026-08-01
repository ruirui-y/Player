#include "BitrateController.h"
#include "Common/Network/NetworkStats.h"
#include "Common/LogManager.h"
#include <algorithm>

BitrateController::BitrateController(int initial_kbps, int min_kbps, int max_kbps)
    : min_kbps_(min_kbps)
    , max_kbps_(max_kbps)
    , target_bitrate_(initial_kbps)
    , consecutive_low_loss_(0)
{
    LogManager::Log("INFO", "[BitrateCtrl] 初始化, 目标码率=%d kbps, 范围=[%d, %d]",
                    target_bitrate_, min_kbps_, max_kbps_);
}

int BitrateController::OnStatsReport(const NetworkStats& stats)
{
    auto now = std::chrono::steady_clock::now();

    // ---- 防抖：距上次调整不足 ADJUST_INTERVAL_MS 不调整 ----
    auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_adjust_time_).count();
    if (since_last < ADJUST_INTERVAL_MS)
        return target_bitrate_;                                 // 太快，不调整

    float loss_rate = stats.loss_rate;

    // ---- 下降：丢包率高 → 快速降码率 ----
    if (loss_rate > HIGH_LOSS_THRESHOLD ||
        (loss_rate > MEDIUM_LOSS_THRESHOLD && consecutive_low_loss_ == 0))
    {
        int new_bitrate = static_cast<int>(target_bitrate_ * DECREASE_FACTOR);
        new_bitrate = std::max(new_bitrate, min_kbps_);

        if (new_bitrate < target_bitrate_)
        {
            LogManager::Log("WARN", "[BitrateCtrl] 高丢包 %.1f%%, 降码率: %d → %d kbps",
                            loss_rate, target_bitrate_, new_bitrate);
            target_bitrate_ = new_bitrate;
            last_adjust_time_ = now;
            last_decrease_time_ = now;
            consecutive_low_loss_ = 0;
        }
    }
    // ---- 上升：丢包率低 + RTT 正常 → 缓慢升码率 ----
    else if (loss_rate < LOW_LOSS_THRESHOLD)
    {
        consecutive_low_loss_++;

        if (consecutive_low_loss_ >= CONSECUTIVE_LOW_FOR_INCREASE)
        {
            // 确认稳定低丢包，增加码率
            int new_bitrate = static_cast<int>(target_bitrate_ * INCREASE_FACTOR);

            // 恢复时不要超过下降前的码率（保守策略：每次只升 5%，不设 hard max）
            new_bitrate = std::min(new_bitrate, max_kbps_);

            if (new_bitrate > target_bitrate_)
            {
                LogManager::Log("INFO", "[BitrateCtrl] 低丢包 %.1f%%（连续%d次）, 升码率: %d → %d kbps",
                                loss_rate, consecutive_low_loss_, target_bitrate_, new_bitrate);
                target_bitrate_ = new_bitrate;
                last_adjust_time_ = now;
                consecutive_low_loss_ = 0;
            }
        }
    }
    else
    {
        // 丢包率在 1%-5% 之间，维持当前码率
        consecutive_low_loss_ = 0;
    }

    return target_bitrate_;
}
