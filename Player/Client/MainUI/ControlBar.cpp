#include "ControlBar.h"
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QStyle>

ControlBar::ControlBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ControlBar");

    // 播放/暂停按钮
    play_btn_ = new QPushButton("▶", this);
    play_btn_->setObjectName("PlayBtn");
    play_btn_->setFixedSize(40, 32);

    // 进度条
    seek_bar_ = new QSlider(Qt::Horizontal, this);
    seek_bar_->setObjectName("SeekBar");
    seek_bar_->setRange(0, 0);

    // 时间标签
    time_label_ = new QLabel("00:00 / 00:00", this);
    time_label_->setObjectName("TimeLabel");

    // 音量滑块
    vol_slider_ = new QSlider(Qt::Horizontal, this);
    vol_slider_->setObjectName("VolSlider");
    vol_slider_->setRange(0, 100);
    vol_slider_->setValue(100);
    vol_slider_->setFixedWidth(80);

    // 文件浏览器切换按钮
    folder_btn_ = new QPushButton(u8"📁", this);
    folder_btn_->setObjectName("FolderBtn");
    folder_btn_->setToolTip(u8"文件浏览器");
    folder_btn_->setFixedSize(32, 32);

    // ---- 水平布局 ----
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);
    layout->addWidget(play_btn_);
    layout->addWidget(seek_bar_, 1);
    layout->addWidget(time_label_);
    layout->addWidget(vol_slider_);
    layout->addWidget(folder_btn_);

    // 信号
    QObject::connect(folder_btn_, &QPushButton::clicked, this, [this]()
        {
            emit SigToggleFileBrowser();
        });

    seek_bar_->installEventFilter(this);
}

bool ControlBar::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == seek_bar_ && event->type() == QEvent::MouseButtonPress)
    {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton)
        {
            int val = QStyle::sliderValueFromPosition(
                seek_bar_->minimum(), seek_bar_->maximum(),
                me->pos().x(), seek_bar_->width());
            seek_bar_->setValue(val);
            emit SigSeekRequested(seek_bar_->value());
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}
