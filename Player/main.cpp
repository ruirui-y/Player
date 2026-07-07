#include "MainUI/MainWindow.h"
#include "Core/FFmpegPlayer.h"
#include <QtWidgets/QApplication>
#include <QTimer>
#include <QDebug>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);                       // 没有可见窗口时不让程序退出

    // ---- 创建渲染窗口 ----
    MainWindow window;
    window.SetVideoRect(100, 100, 1280, 720);                   // 画面出现在屏幕 (100,100)，宽1280高720
    window.show();                                              // 窗口要先 show 才能拿到有效的 HWND

    // ---- 创建播放核心 ----
    FFmpegPlayer player;

    // 把窗口句柄传给播放器（D3D11 交换链需要）
    player.SetVideoHwnd(window.GetVideoHwnd());

    // 软解回退时，解码帧以 QImage 形式传给 MainWindow 用 QLabel 显示
    QObject::connect(&player, &FFmpegPlayer::SigFrameReady,
        &window, &MainWindow::OnFrameReady);

    // ---- 以下为播放器状态信号的日志输出 ----
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

    // ---- 打开文件并播放 ----
    QString videoPath = "H:/YJJ/Project/Player/Player/Movie/hevc_8k60P_bilibili_1.mp4";
    // QString videoPath = "H:/YJJ/Project/Player/Player/Movie/huanyou.mp4";  // 另一个测试视频
    if (player.OpenFile(videoPath))
    {
        // 等 500ms 让 loaded 回调先触发，再播放
        QTimer::singleShot(500, [&]()
            {
                player.Play();
            });
    }
    else
    {
        qDebug("文件打开失败: %s", qPrintable(videoPath));
    }

    // 程序退出时安全释放所有资源
    QObject::connect(&app, &QApplication::aboutToQuit, [&]()
        {
            player.Close();
        });

    return app.exec();
}