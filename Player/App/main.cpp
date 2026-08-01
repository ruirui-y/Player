#include "PlayerApp.h"
#include "Common/LogManager.h"

#include <QtWidgets/QApplication>
#include <windows.h>

// ================================================================
// ---- main：统一启动 ----
//
//   Player.exe → LaunchPage → 用户选择模式（播放/控制/被控）
//   不再支持命令行参数，所有配置由 UI 完成
// ================================================================
int main(int argc, char* argv[])
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    LogManager::Init();

    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setQuitOnLastWindowClosed(false);

    PlayerApp player_app;
    player_app.Init();

    int ret = app.exec();
    CoUninitialize();
    LogManager::Shutdown();
    return ret;
}
