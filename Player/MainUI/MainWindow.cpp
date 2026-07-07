#include "MainWindow.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // 注意：不能设置 WA_NativeWindow / WA_PaintOnScreen 等属性
    // 否则 Qt 的 QLabel 绘制会被覆盖，软解回退时画面出不来
    // Qt 自己管理渲染引擎即可

    // 无边框窗口 + 不出现在任务栏
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setStyleSheet("background-color: black;");

    // 创建黑色背景的 QLabel，软解回退时显示解码图像
    video_label_ = new QLabel(this);
    video_label_->setAlignment(Qt::AlignCenter);
    video_label_->setStyleSheet("background-color: black;");
    video_label_->hide();  // 先隐藏，等第一帧到了再显示，避免启动闪烁
}

MainWindow::~MainWindow()
{
}

// 设置画面在屏幕上的位置和尺寸
void MainWindow::SetVideoRect(int x, int y, int w, int h)
{
    setGeometry(x, y, w, h);
    if (video_label_)
    {
        video_label_->setGeometry(0, 0, w, h);  // QLabel 填满整个窗口
    }
}

// 返回窗口句柄，D3D11 创建交换链时需要绑定到这个窗口上
HWND MainWindow::GetVideoHwnd() const
{
    return (HWND)this->winId();
}

// 软解回退时，接收一帧 QImage 并显示到 QLabel 上
void MainWindow::OnFrameReady(const QImage& image)
{
    if (video_label_)
    {
        // 第一帧到达前 QLabel 是隐藏的，收到第一帧时显示它
        if (video_label_->isHidden())
        {
            video_label_->show();
        }

        QPixmap pixmap = QPixmap::fromImage(image);
        // 用 FastTransformation 代替 SmoothTransformation
        // 4K 图像用 Smooth 缩放会消耗大量 CPU 时间，导致主线程卡顿
        video_label_->setPixmap(pixmap.scaled(
            video_label_->size(),
            Qt::KeepAspectRatio,
            Qt::FastTransformation));
    }
}