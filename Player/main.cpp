#include "PlayerApp.h"
#include <QtWidgets/QApplication>
#include <windows.h>

int main(int argc, char* argv[])
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setQuitOnLastWindowClosed(false);

    PlayerApp player_app;
    player_app.Init(argc, argv);

    int ret = app.exec();
    CoUninitialize();           // 程序退出时释放
    return ret;
}