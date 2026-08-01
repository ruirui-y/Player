#ifndef PLAYERPAGE_H
#define PLAYERPAGE_H

#include <QWidget>
#include <QSplitter>

class VideoWidget;
class ControlBar;
class FileBrowser;

// 播放器页面（纯 UI 容器）
// 不持有 FFmpegPlayer，播放逻辑由 PlayerApp 通过信号绑定实现
class PlayerPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlayerPage(QWidget* parent = nullptr);

    VideoWidget* GetVideoWidget() const { return video_widget_; }
    ControlBar*  GetControlBar()  const { return control_bar_; }
    FileBrowser* GetFileBrowser() const { return file_browser_; }

    void ToggleFileBrowser();           // 切换文件浏览器显示/隐藏

signals:
    void SigBack();                     // 返回启动页

private:
    VideoWidget* video_widget_{nullptr};
    ControlBar*  control_bar_{nullptr};
    FileBrowser* file_browser_{nullptr};
    QSplitter*   splitter_{nullptr};    // 文件浏览器 | 视频区域
};

#endif // PLAYERPAGE_H
