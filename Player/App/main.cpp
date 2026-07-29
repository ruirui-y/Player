#include "PlayerApp.h"
#include "Server/StreamServer.h"

#include <QtWidgets/QApplication>
#include <windows.h>
#include <QDebug>
#include <cstring>
#include <cstdlib>

// ==== 辅助函数：命令行参数解析 ====

// 检查是否存在某个 flag 参数
static bool HasArg(const char* key, int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], key) == 0)
            return true;
    }
    return false;
}

// 获取某个 key 参数的值，找不到则返回默认值
static const char* GetArgValue(const char* key, int argc, char* argv[],
                               const char* default_val = nullptr)
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (std::strcmp(argv[i], key) == 0)
            return argv[i + 1];
    }
    return default_val;
}

// ==== Ctrl+C 信号处理（服务器模式用） ====
static StreamServer* g_server_ptr = nullptr;

static BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT)
    {
        if (g_server_ptr)
            g_server_ptr->Stop();
        return TRUE;
    }
    return FALSE;
}

// ================================================================
// ---- main：三种启动模式 ----
//
//   1. 文件播放器（默认）：  Player.exe
//   2. 服务器模式：          Player.exe --server --port 47998 --monitor 1
//   3. 客户端串流模式：      Player.exe --stream --port 47998 --width 1920 --height 1080 --fps 60
// ================================================================
int main(int argc, char* argv[])
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 服务器模式：不走 Qt 事件循环，直接阻塞推流 ----
    if (HasArg("--server", argc, argv))
    {
        uint16_t port = (uint16_t)std::atoi(GetArgValue("--port", argc, argv, "47998"));
        int monitor_index = std::atoi(GetArgValue("--monitor", argc, argv, "1"));
        const char* dest_ip = GetArgValue("--ip", argc, argv, "127.0.0.1");
        int fps = std::atoi(GetArgValue("--fps", argc, argv, "60"));
        int bitrate = std::atoi(GetArgValue("--bitrate", argc, argv, "10000"));

        SetConsoleCtrlHandler(ConsoleHandler, TRUE);

        StreamServer server;
        g_server_ptr = &server;

        if (server.Init(port, monitor_index, dest_ip, fps, bitrate))
        {
            server.Run();
        }
        else
        {
            printf("[main] 服务器初始化失败\n");
        }

        g_server_ptr = nullptr;
        CoUninitialize();
        return 0;
    }

    // ---- 播放器 / 串流模式：需要 Qt 事件循环 ----
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setQuitOnLastWindowClosed(false);

    PlayerApp player_app;
    player_app.Init(argc, argv);

    int ret = app.exec();
    CoUninitialize();
    return ret;
}
