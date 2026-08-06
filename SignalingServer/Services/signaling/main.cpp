#include "SignalingApp.h"
#include <csignal>

static std::atomic<bool> g_stop{ false };

static void SignalHandler(int)
{
    g_stop = true;
}

int main()
{
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    SignalingApp app;
    if (!app.Init())
        return -1;

    app.Start();    // 阻塞在 io_ctx_.run()

    // 信号触发后退出
    while (!g_stop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return 0;
}
