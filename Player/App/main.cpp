#include "PlayerApp.h"
#include "Server/StreamServer.h"
#include "Common/LogManager.h"

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
//   2. 服务器模式：          Player.exe --server --port 47998 --monitor 1 --ctrl-port 47989
//   3. 客户端串流模式：      Player.exe --stream --port 47998 --ip 127.0.0.1 --ctrl-port 47989
// ================================================================
int main(int argc, char* argv[])
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 服务器模式：不走 Qt 事件循环，直接阻塞推流 ----
    if (HasArg("--server", argc, argv))
    {
        // ---- 先分配控制台，再初始化日志，这样 fopen 失败能看到诊断 ----
        AllocConsole();
        FILE* con_out = nullptr;
        freopen_s(&con_out, "CONOUT$", "w", stdout);
        freopen_s(&con_out, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);                 // 控制台 UTF-8 编码，解决中文乱码

        // ---- 初始化日志系统（控制台已就绪，能看到 fopen 失败的错误） ----
        LogManager::Init();
        LogManager::SetConsoleEcho(true);
        LogManager::Log("INFO", "[main] 服务器模式启动，参数个数=%d", argc);

        uint16_t port = (uint16_t)std::atoi(GetArgValue("--port", argc, argv, "47998"));
        int monitor_index = std::atoi(GetArgValue("--monitor", argc, argv, "1"));
        const char* dest_ip = GetArgValue("--ip", argc, argv, "127.0.0.1");
        int fps = std::atoi(GetArgValue("--fps", argc, argv, "120"));
        int bitrate = std::atoi(GetArgValue("--bitrate", argc, argv, "10000"));
        uint16_t ctrl_port = (uint16_t)std::atoi(GetArgValue("--ctrl-port", argc, argv, "47989"));
        bool use_fast = HasArg("--fast", argc, argv);

        LogManager::Log("INFO", "[main] port=%d  monitor=%d  ip=%s  fps=%d  bitrate=%d  ctrl=%d  fast=%d",
                        port, monitor_index, dest_ip, fps, bitrate, ctrl_port, use_fast);

        SetConsoleCtrlHandler(ConsoleHandler, TRUE);

        StreamServer server;
        g_server_ptr = &server;

        if (server.Init(port, monitor_index, dest_ip, fps, bitrate, ctrl_port, use_fast))
        {
            server.Run();
        }
        else
        {
            LogManager::Log("ERR", "[main] 服务器初始化失败");
        }

        // ---- 等待用户按键再关闭控制台，防止闪退看不到错误 ----
        LogManager::Log("INFO", "[main] 按回车键退出...");

        g_server_ptr = nullptr;
        CoUninitialize();
        LogManager::Shutdown();
        return 0;
    }

    // ---- 播放器 / 串流模式：需要 Qt 事件循环 ----
    LogManager::Init();                            // 播放器模式：在 QApplication 之前初始化日志

    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setQuitOnLastWindowClosed(false);

    PlayerApp player_app;
    player_app.Init(argc, argv);

    int ret = app.exec();
    CoUninitialize();
    LogManager::Shutdown();
    return ret;
}
