#ifndef STREAMWINDOW_H
#define STREAMWINDOW_H

#include <QWidget>
#include <QTimer>

class TitleBar;
class StreamVideoWidget;
class QLabel;

// 串流模式主窗口
// 与播放器的 MainWindow 完全独立，不含 ControlBar / FileBrowser / SeekBar
// 布局：TitleBar + StreamVideoWidget + 底部状态栏
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

signals:
    void SigRequestClose();                                                 // 请求关闭

public slots:
    void ToggleFullScreen();                                                // 全屏/窗口切换

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void OnMouseIdle();                                                     // 鼠标闲置隐藏控件

private:
    TitleBar* title_bar_{ nullptr };                                        // 标题栏（复用播放器的 TitleBar）
    StreamVideoWidget* video_widget_{ nullptr };                            // 视频显示区
    QLabel* status_label_{ nullptr };                                       // 底部状态栏（连接状态/分辨率/帧率）
    QTimer* mouse_idle_timer_{ nullptr };                                   // 鼠标闲置定时器
    bool fullscreen_{ false };                                              // 是否全屏
};

#endif // STREAMWINDOW_H
