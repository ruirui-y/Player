#include "VideoWidget.h"

VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent)
{
    // 强制创建独立的原生窗口句柄（否则 winId() 可能复用父窗口的）
    setAttribute(Qt::WA_NativeWindow);
    // 纯黑背景，D3D11 画上去之前不会闪烁白屏
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setMinimumSize(320, 180);

    // 创建软解回退用的 QLabel
    video_label_ = new QLabel(this);
    video_label_->setAlignment(Qt::AlignCenter);
    video_label_->setStyleSheet("background-color: black;");
    video_label_->hide();  // 先隐藏，等第一帧到了再显示
}

HWND VideoWidget::GetVideoHwnd() const
{
    return (HWND)winId();
}

void VideoWidget::ClearFrame()
{
    if (video_label_)
    {
        video_label_->clear();                      // 清除 QLabel 内容
        video_label_->setText("");                  // 确保是空
    }
}

void VideoWidget::OnFrameReady(const QImage& image)
{
    if (!video_label_) return;

    if (video_label_->isHidden())
        video_label_->show();

    // 调整 QLabel 大小填满 VideoWidget
    video_label_->setGeometry(0, 0, width(), height());

    QPixmap pixmap = QPixmap::fromImage(image);
    video_label_->setPixmap(pixmap.scaled(
        video_label_->size(),
        Qt::KeepAspectRatio,
        Qt::FastTransformation));
}