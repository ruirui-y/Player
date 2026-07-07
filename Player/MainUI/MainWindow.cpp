#include "MainWindow.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // 【修改 1】注释或删除这四行！既然目前走 QLabel 软渲染，就必须让 Qt 掌控渲染引擎
    // setAttribute(Qt::WA_NativeWindow, true);
    // setAttribute(Qt::WA_PaintOnScreen, true);
    // setAttribute(Qt::WA_OpaquePaintEvent, true);
    // setAttribute(Qt::WA_NoSystemBackground, true);

    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setStyleSheet("background-color: black;");

    video_label_ = new QLabel(this);
    video_label_->setAlignment(Qt::AlignCenter);
    video_label_->setStyleSheet("background-color: black;");
    video_label_->hide();
}

MainWindow::~MainWindow()
{
}

void MainWindow::SetVideoRect(int x, int y, int w, int h)
{
    setGeometry(x, y, w, h);
    if (video_label_)
    {
        video_label_->setGeometry(0, 0, w, h);
    }
}

HWND MainWindow::GetVideoHwnd() const
{
    return (HWND)this->winId();
}

void MainWindow::OnFrameReady(const QImage& image)
{
    if (video_label_)
    {
        if (video_label_->isHidden()) {
            video_label_->show();
        }

        QPixmap pixmap = QPixmap::fromImage(image);
        // 【修改 2】千万别在主线程对 4K 图像做 Smooth 缩放！改成 Qt::FastTransformation
        video_label_->setPixmap(pixmap.scaled(
            video_label_->size(),
            Qt::KeepAspectRatio,
            Qt::FastTransformation));
    }
}