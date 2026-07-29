#ifndef CLOCKUTIL_H
#define CLOCKUTIL_H

#include <algorithm>
#include <cmath>

// 参考 ffplay 的同步阈值常量
constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;                              // 最小同步阈值（秒）
constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;                               // 最大同步阈值（秒）
constexpr double AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;                          // 帧重复阈值（秒）
constexpr double AV_NOSYNC_THRESHOLD = 10.0;                                // 失同步阈值（秒）

// 计算视频帧应该延迟多久显示
// delay: 帧的天然间隔（秒），如 25fps = 0.04s
// diff:  video_pts - audio_clock（秒），正=视频快了，负=视频慢了
inline double ComputeTargetDelay(double delay, double diff)
{
    if (std::isnan(diff) || std::fabs(diff) >= AV_NOSYNC_THRESHOLD)
        return delay;

    // 动态阈值：介于 0.04~0.1 之间，随 delay 变化
    double sync_threshold = std::max(
        AV_SYNC_THRESHOLD_MIN,
        std::min(AV_SYNC_THRESHOLD_MAX, delay));

    if (diff <= -sync_threshold)
    {
        // 视频落后（diff < 0），加速追赶
        delay = std::max(0.0, delay + diff);
    }
    else if (diff >= sync_threshold)
    {
        if (delay > AV_SYNC_FRAMEDUP_THRESHOLD)
        {
            // 帧时长大，温和等待
            delay = delay + diff;
        }
        else
        {
            // 帧时长小（<=0.1s），激进减速：翻倍
            delay = 2 * delay;
        }
    }
    // |diff| < sync_threshold → 不做调整

    return delay;
}

#endif // CLOCKUTIL_H
