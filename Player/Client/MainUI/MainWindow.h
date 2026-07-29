#pragma once

#include <QWidget>
#include <QImage>
#include <QTimer>
#include <QSplitter>

class TitleBar;
class VideoWidget;
class ControlBar;
class FileBrowser;

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
    FileBrowser* GetFileBrowser() const { return file_browser_; }

    HWND GetVideoHwnd() const;

    void ToggleFileBrowser();                                                       // 切换文件浏览器显示/隐藏

signals:
    void SigRequestClose();
    void SigFileDropped(const QString& path);                                       // 拖拽文件到窗口

public slots:
    void ToggleFullScreen();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    // 拖放支持
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void OnMouseIdle();

private:
    TitleBar* title_bar_{ nullptr };
    VideoWidget* video_widget_{ nullptr };
    ControlBar* control_bar_{ nullptr };
    FileBrowser* file_browser_{ nullptr };
    QSplitter* splitter_{ nullptr };
    QTimer* mouse_idle_timer_{ nullptr };
    bool          fullscreen_{ false };
};
