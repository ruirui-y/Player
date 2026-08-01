#include "Pacer.h"

int64_t Pacer::NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Pacer::OnFramePresented()
{
    if (!running_) return;

    ++frame_count_;
    int64_t now = NowUs();

    if (now - fps_start_ >= 1000000)        // 每秒统计一次
    {
        render_fps_.store(frame_count_);
        frame_count_ = 0;
        fps_start_ = now;
    }
}
