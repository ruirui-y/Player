#include "MainWindow.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    // 创建一个 QLabel 填满 centralWidget，用来显示视频画面
    video_label_ = new QLabel(ui.centralWidget);
    video_label_->setGeometry(0, 0, width(), height());
    video_label_->setAlignment(Qt::AlignCenter);
    // 黑背景，没有视频帧时显示黑色
    video_label_->setStyleSheet("background-color: black;");
}

MainWindow::~MainWindow()
{
}

// 设置画面位置和尺寸
void MainWindow::SetVideoRect(int x, int y, int w, int h)
{
    setGeometry(x, y, w, h);
    if (video_label_)
    {
        video_label_->setGeometry(0, 0, w, h);
    }
}

// 收到解码线程发来的一帧画面，显示到 QLabel 上
void MainWindow::OnFrameReady(const QImage& image)
{
    if (video_label_)
    {
        // 缩放图片适应 QLabel 大小，保持宽高比
        QPixmap pixmap = QPixmap::fromImage(image);
        video_label_->setPixmap(pixmap.scaled(
            video_label_->size(),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    }
}