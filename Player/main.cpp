#include "MainUI/MainWindow.h"
#include "Core/FFmpegPlayer.h"          // 改这里
#include <QtWidgets/QApplication>
#include <QTimer>
#include <QDebug>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    MainWindow window;
    window.SetVideoRect(100, 100, 1280, 720);
    window.show();

    FFmpegPlayer player;                 // 改这里

    // 把窗口句柄传给播放器
    player.SetVideoHwnd(window.GetVideoHwnd());

    // 软解回退时，画面通过这个信号显示
    QObject::connect(&player, &FFmpegPlayer::SigFrameReady,   // 改这里
        &window, &MainWindow::OnFrameReady);

    QObject::connect(&player, &FFmpegPlayer::SigLoaded, [](qint64 duration_ms)
        {
            qDebug("文件加载完成，时长: %lld ms", duration_ms);
        });

    QObject::connect(&player, &FFmpegPlayer::SigFinished, [&]()
        {
            qDebug("播放结束");
        });

    QObject::connect(&player, &FFmpegPlayer::SigError, [](const QString& msg)
        {
            qDebug("错误: %s", qPrintable(msg));
        });

    QObject::connect(&player, &FFmpegPlayer::SigPlayState, [](const QString& state)
        {
            qDebug("状态变化: %s", qPrintable(state));
        });

    QString videoPath = "H:/YJJ/Project/Player/Player/Movie/huanyou.mp4";
    if (player.OpenFile(videoPath))
    {
        QTimer::singleShot(500, [&]()
            {
                player.Play();
            });
    }
    else
    {
        qDebug("文件打开失败: %s", qPrintable(videoPath));
    }

    QObject::connect(&app, &QApplication::aboutToQuit, [&]()
        {
            player.Close();
        });

    return app.exec();
}