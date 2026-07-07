#pragma once

#include <QtWidgets/QMainWindow>
#include <QLabel>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    void SetVideoRect(int x, int y, int w, int h);   // 设置画面位置和尺寸
    HWND GetVideoHwnd() const;                        // 返回窗口句柄，给 D3D11 用

    // 软解回退时仍然需要显示 QImage
public slots:
    void OnFrameReady(const QImage& image);

private:
    QLabel* video_label_{ nullptr };
};