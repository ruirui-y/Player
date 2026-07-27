#include "ControlBar.h"
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QStyle>

ControlBar::ControlBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(40);

    // ---- 播放/暂停按钮 ----
    play_btn_ = new QPushButton("▶", this);
    play_btn_->setObjectName("PlayBtn");
    play_btn_->setFixedSize(30, 30);

    // ---- 进度条 ----（安装事件过滤器拦截点击轨道）
    seek_bar_ = new QSlider(Qt::Horizontal, this);
    seek_bar_->setObjectName("SeekBar");
    seek_bar_->setRange(0, 0);
    seek_bar_->installEventFilter(this);

    // ---- 时间标签 ----
    time_label_ = new QLabel("00:00 / 00:00", this);
    time_label_->setObjectName("TimeLabel");
    time_label_->setFixedWidth(140);

    // ---- 音量滑块 ----
    vol_slider_ = new QSlider(Qt::Horizontal, this);
    vol_slider_->setObjectName("VolSlider");
    vol_slider_->setRange(0, 100);
    vol_slider_->setValue(70);
    vol_slider_->setFixedWidth(80);

    // ---- 布局 ----
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(8);
    layout->addWidget(play_btn_);
    layout->addWidget(seek_bar_, 1);
    layout->addWidget(time_label_);
    layout->addWidget(vol_slider_);
}

// 拦截 seek_bar 的鼠标释放事件，处理轨道点击
bool ControlBar::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == seek_bar_ && event->type() == QEvent::MouseButtonRelease)
    {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton)
        {
            // 根据点击位置计算进度条值
            int val = QStyle::sliderValueFromPosition(
                seek_bar_->minimum(), seek_bar_->maximum(),
                me->pos().x(), seek_bar_->width());
            seek_bar_->setValue(val);
            emit SigSeekRequested(val);
        }
    }
    return QWidget::eventFilter(obj, event);
}