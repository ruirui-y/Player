#ifndef PLAYERAPP_H
#define PLAYERAPP_H

#include <QObject>
#include <cstdint>

class MainWindow;
class StreamWindow;
class FFmpegPlayer;

// 应用程序控制器
// 根据命令行参数决定启动模式：
//   1. 文件播放器（默认）：创建 MainWindow + FFmpegPlayer
//   2. 串流客户端：创建 StreamWindow + FFmpegPlayer
//
// main.cpp 只需要创建 PlayerApp 实例，调 Init()，然后 app.exec()
class PlayerApp : public QObject
{
    Q_OBJECT

public:
    explicit PlayerApp(QObject* parent = nullptr);
    ~PlayerApp();

    // 初始化：解析命令行参数，选择启动模式
    bool Init(int argc, char* argv[]);

private:
    // ==== 通用 ====
    void LoadStyle();                                           // 加载 style.qss

    // ==== 文件播放器模式 ====
    void CreatePlayerUI();                                      // 创建 MainWindow
    void CreatePlayer();                                        // 创建 FFmpegPlayer 并绑定到 MainWindow
    void BindPlayerSignals();                                   // 绑定播放器控制信号
    void StartProgressTimer();                                  // 启动进度轮询定时器
    void UpdateProgress();                                      // 更新进度条 + 时间标签
    void OpenFile(const QString& path);                         // 打开并播放文件
    void OnFileSelected(const QString& path);                   // 文件浏览器选定文件
    void OnSelectFile();                                        // 弹出文件选择对话框
    void TestScreenCapture();                                   // 测试桌面捕获是否成功

    // ==== 串流客户端模式 ====
    void CreateStreamUI();                                      // 创建 StreamWindow
    void CreateStreamPlayer();                                  // 创建 FFmpegPlayer 并绑定到 StreamWindow
    void BindStreamSignals();                                   // 绑定串流关闭信号
    void OpenStream(uint16_t port, int fps);                              // 打开网络串流并自动播放

private:
    // ==== 当前模式的窗口（二选一） ====
    MainWindow* main_window_{ nullptr };                        // 文件播放器窗口
    StreamWindow* stream_window_{ nullptr };                    // 串流客户端窗口

    // ==== 播放器引擎（两种模式共用） ====
    FFmpegPlayer* player_{ nullptr };
    QTimer* progress_timer_{ nullptr };                         // 进度轮询定时器（仅文件模式）

    // ==== 串流模式参数 ====
    bool is_streaming_{ false };                                // 是否串流模式
};

#endif // PLAYERAPP_H
