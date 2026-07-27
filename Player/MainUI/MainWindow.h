#pragma once

#include <QWidget>
#include <QImage>
#include <QTimer>

class TitleBar;
class VideoWidget;
class ControlBar;

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    void SetVideoRect(int x, int y, int w, int h);
    void OnFrameReady(const QImage& image);

    VideoWidget* GetVideoWidget() const { return video_widget_; }
    ControlBar* GetControlBar()  const { return control_bar_; }
    TitleBar* GetTitleBar()    const { return title_bar_; }

    HWND GetVideoHwnd() const;

signals:
    void SigRequestClose();

public slots:
    void ToggleFullScreen();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void OnMouseIdle();

private:
    TitleBar* title_bar_{ nullptr };
    VideoWidget* video_widget_{ nullptr };
    ControlBar* control_bar_{ nullptr };
    QTimer* mouse_idle_timer_{ nullptr };
    bool          fullscreen_{ false };
};