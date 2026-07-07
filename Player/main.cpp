#include "MainUI/MainWindow.h"
#include "Core/FFmpegCore.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);       // 没有可见窗口时不退出

    // ---- 创建渲染窗口 ----
    MainWindow window;
    window.SetVideoRect(100, 100, 1280, 720);   // 画面出现在 (100,100)，宽1280高720
    window.show();                               // 第一步先显示窗口，后面再隐藏

    // ---- 创建播放核心 ----
    FFmpegCore player;

    // ---- 连接信号：解码线程每帧画面 → 窗口显示 ----
    QObject::connect(&player, &FFmpegCore::SigFrameReady,
        &window, &MainWindow::OnFrameReady);

    // ---- 连接信号：状态变化 ----
    QObject::connect(&player, &FFmpegCore::SigLoaded, [](qint64 duration_ms)
        {
            qDebug("文件加载完成，时长: %lld ms", duration_ms);
        });

    QObject::connect(&player, &FFmpegCore::SigFinished, [&]()
        {
            qDebug("播放结束");
        });

    QObject::connect(&player, &FFmpegCore::SigError, [](const QString& msg)
        {
            qDebug("错误: %s", qPrintable(msg));
        });

    QObject::connect(&player, &FFmpegCore::SigPlayState, [](const QString& state)
        {
            qDebug("状态变化: %s", qPrintable(state));
        });

    // ---- 打开文件并播放（这里硬编码一个路径，你自己改） ----
    QString videoPath = "H:/YJJ/Project/Player/Player/Movie/huanyou.mp4";          

    if (player.OpenFile(videoPath))
    {
        // 延迟 500ms 再播放，让你能看到 loaded 回调先触发
        QTimer::singleShot(500, [&]()
            {
                player.Play();
            });
    }
    else
    {
        qDebug("文件打开失败: %s", qPrintable(videoPath));
    }

    // ---- 可以按键盘退出（演示用） ----
    // 按 Q 键退出程序
    QObject::connect(&app, &QApplication::aboutToQuit, [&]()
        {
            player.Close();
        });

    return app.exec();
}