#ifndef STREAMVIDEOWIDGET_H
#define STREAMVIDEOWIDGET_H

#include <QWidget>
#include <QLabel>

// 串流视频显示组件
// 与播放器的 VideoWidget 独立，专用于远程串流场景
// 提供 HWND 给 D3D11 交换链；软解回退时用 QLabel 显示 QImage
class StreamVideoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StreamVideoWidget(QWidget* parent = nullptr);

    HWND GetVideoHwnd() const;                          // 返回原生窗口句柄，给 D3D11 创建交换链

public slots:
    void OnFrameReady(const QImage& image);             // 软解回退时显示一帧
    void ClearFrame();                                  // 清空画面

protected:
    void resizeEvent(QResizeEvent* event) override;     // QLabel 跟随父窗口缩放

private:
    QLabel* video_label_{ nullptr };                    // 软解回退时显示画面
};

#endif // STREAMVIDEOWIDGET_H
