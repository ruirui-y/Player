#ifndef PLAYERAPP_H
#define PLAYERAPP_H

#include <QObject>
#include <cstdint>
#include <QString>

class MainWindow;
class FFmpegPlayer;

// 应用控制器（第七阶段重构：只管理引擎，不碰 UI 堆栈）
// Init() 创建 MainWindow，之后 UI 交互全部由 MainWindow 管理
// PlayerApp 只对外提供 StartPlayer / StartStream / DestroyPlayer 引擎接口
class PlayerApp : public QObject
{
    Q_OBJECT

public:
    explicit PlayerApp(QObject* parent = nullptr);
    ~PlayerApp();

    void Init();                                                // 创建 MainWindow → 显示

    // ---- 引擎管理（由 MainWindow 页面调用） ----
    void StartPlayer(const QString& path = QString());          // 播放器模式：创建 FFmpegPlayer + 绑定 PlayerPage
    void StartStream(const QString& ip, uint16_t port,
                     uint16_t ctrl_port, int fps);              // 控制端模式：创建 FFmpegPlayer + 启动串流
    void DestroyPlayer();                                       // 销毁播放器引擎

    // 控制端：输入转发启动（由 MainWindow 延迟触发）
    void OnStreamReady(const QString& ip, uint16_t ctrl_port);
    bool HasSenderIP() const;                               // 是否已收到服务端视频包

private:
    void LoadStyle();

    // 播放器引擎绑定
    void BindPlayerSignals();                                   // PlayerPage UI ↔ FFmpegPlayer
    void BindStreamSignals();                                   // StreamWindow UI ↔ FFmpegPlayer

    // 进度控制
    void StartProgressTimer();
    void UpdateProgress();

    MainWindow*   main_window_{nullptr};
    FFmpegPlayer* player_{nullptr};
    QTimer*       progress_timer_{nullptr};

    // 串流参数（OnStreamReady 时使用）
    QString  stream_ip_;
    uint16_t stream_ctrl_port_{0};
};

#endif // PLAYERAPP_H
