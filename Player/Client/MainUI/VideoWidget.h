#ifndef VIDEOWIDGET_H
#define VIDEOWIDGET_H

#include <QWidget>
#include <QLabel>
#include <QImage>
#include <QPixmap>

// 视频渲染区域
// 提供原生 HWND 给 D3D11 渲染；软解回退时使用 QLabel 显示 QImage
class VideoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);

    HWND GetVideoHwnd() const;                          // 返回原生窗口句柄，给 D3D11 创建交换链

public slots:
    void OnFrameReady(const QImage& image);             // 软解回退时显示一帧
    void ClearFrame();                                  // 清空画面

private:
    QLabel* video_label_{ nullptr };                    // 软解回退时显示画面
};

#endif // VIDEOWIDGET_H
