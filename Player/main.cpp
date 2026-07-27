#include "PlayerApp.h"
#include <QtWidgets/QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    PlayerApp player_app;
    player_app.Init(argc, argv);

    return app.exec();
}