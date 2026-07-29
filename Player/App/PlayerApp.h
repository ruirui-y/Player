#ifndef PLAYERAPP_H
#define PLAYERAPP_H

#include <QObject>

class MainWindow;
class FFmpegPlayer;

// 应用程序初始化类
// main.cpp 只需要创建 PlayerApp 实例，调 Init()，然后 app.exec()
class PlayerApp : public QObject
{
    Q_OBJECT

public:
    explicit PlayerApp(QObject* parent = nullptr);
    ~PlayerApp();

    // 初始化所有组件：加载样式 → 创建窗口 → 创建播放器 → 绑定信号 → 打开文件
    bool Init(int argc, char* argv[]);

    MainWindow* GetMainWindow() const { return main_window_; }

private:
    void LoadStyle();                                           // 加载 style.qss
    void CreateUI();                                            // 创建窗口和控件
    void CreatePlayer();                                        // 创建播放核心
    void BindSignals();                                         // 绑定控制栏 → 播放器的信号
    void StartProgressTimer();                                  // 启动进度轮询定时器
    void UpdateProgress();                                      // 更新进度条 + 时间标签
    void OpenFile(const QString& path);                         // 打开并播放文件
    void OnFileSelected(const QString& path);                   // 文件浏览器选定文件
    void OnSelectFile();                                        // 弹出文件选择对话框（播放按钮无文件时）
    void TestScreenCapture();                                   // 测试桌面捕获是否成功

private:
    MainWindow* main_window_{ nullptr };
    FFmpegPlayer* player_{ nullptr };
    QTimer* progress_timer_{ nullptr };                         // 进度轮询定时器
};

#endif // PLAYERAPP_H
