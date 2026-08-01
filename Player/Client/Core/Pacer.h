#ifndef PACER_H
#define PACER_H

#include <atomic>
#include <cstdint>
#include <chrono>

// 帧计数器（OSD FPS 统计用）
// 每次 Present 时调用 OnFramePresented()，每秒计算一次 FPS
// Present(1,0) 自身已提供 VSync 同步，Pacer 不再额外等待
class Pacer
{
public:
    Pacer() = default;
    ~Pacer() = default;

    void Start()  { running_ = true;  Reset(); }
    void Stop()   { running_ = false; }
    void Reset()  { frame_count_ = 0; fps_start_ = NowUs(); }

    // 每次 Present 后调用
    void OnFramePresented();

    int  GetRenderFps() const { return render_fps_.load(); }
    bool IsRunning()   const { return running_; }

private:
    static int64_t NowUs();

    std::atomic<bool> running_{false};
    int frame_count_{0};
    int64_t fps_start_{0};
    std::atomic<int> render_fps_{0};
};

#endif // PACER_H
