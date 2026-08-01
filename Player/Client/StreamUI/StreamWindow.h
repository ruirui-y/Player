#ifndef STREAMWINDOW_H
#define STREAMWINDOW_H

#include <QWidget>
#include <QTimer>
#include <cstdint>

class TitleBar;
class StreamVideoWidget;
class InputCollector;
class InputTransportClient;
class FFmpegPlayer;
class QLabel;

// 串流模式主窗口
// 与播放器的 MainWindow 完全独立，不含 ControlBar / FileBrowser / SeekBar
// 布局：TitleBar + StreamVideoWidget + 底部状态栏
// 第三阶段：集成 InputCollector 采集键鼠 + InputTransportClient TCP 发送
class StreamWindow : public QWidget
{
    Q_OBJECT

public:
    StreamWindow(QWidget* parent = nullptr);
    ~StreamWindow();

    void SetVideoRect(int x, int y, int w, int h);                          // 设置窗口位置和大小

    StreamVideoWidget* GetVideoWidget() const { return video_widget_; }     // 获取视频显示组件

    HWND GetVideoHwnd() const;                                              // 获取 D3D11 用的 HWND

    void SetStatusText(const QString& text);                                // 更新底部状态栏文字

    // 启动 OSD 统计定时器（每秒从 FFmpegPlayer 读取 FPS/RTT 刷新状态栏）
    void StartStatsTimer(FFmpegPlayer* player);
    void StopStatsTimer();

    // 第三阶段：启动输入采集 + TCP 控制信道
    // server_ip：服务端 IP（如 "127.0.0.1"）
    // ctrl_port：控制信道端口（如 47989）
    bool StartInput(const char* server_ip, uint16_t ctrl_port);             // 启动输入转发
    void StopInput();                                                       // 停止输入转发

    // 设置为嵌入模式（隐藏内部 TitleBar，由外部 MainWindow 管理标题）
    void SetEmbedded(bool embedded);

    // 获取控制信道客户端（IDR 请求等扩展功能用）
    InputTransportClient* GetInputTransport() const { return input_transport_; }

signals:
    void SigRequestClose();                                                 // 请求关闭

public slots:
    void ToggleFullScreen();                                                // 全屏/窗口切换

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    TitleBar* title_bar_{ nullptr };                                        // 标题栏（复用播放器的 TitleBar）
    StreamVideoWidget* video_widget_{ nullptr };                            // 视频显示区
    QLabel* status_label_{ nullptr };                                       // 底部状态栏（连接状态/分辨率/帧率）
    bool fullscreen_{ false };                                              // 是否全屏

    // ---- 第三阶段：输入转发 ----
    InputCollector* input_collector_{ nullptr };                            // 输入采集器
    InputTransportClient* input_transport_{ nullptr };                      // TCP 输入传输客户端
};

#endif // STREAMWINDOW_H
