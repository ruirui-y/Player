#pragma once

#include <QtWidgets/QMainWindow>
#include <QLabel>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    void SetVideoRect(int x, int y, int w, int h);                                  // 设置画面在桌面的位置和尺寸
    HWND GetVideoHwnd() const;                                                      // 返回窗口句柄，给 D3D11 创建交换链用

public slots:
    void OnFrameReady(const QImage& image);                                         // 软解回退时接收解码帧并显示

private:
    QLabel* video_label_{ nullptr };                                                // 用于显示软解画面的标签（GPU 路径时不使用）
};