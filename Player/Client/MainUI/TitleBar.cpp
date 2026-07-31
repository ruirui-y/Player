#include "TitleBar.h"
#include <QHBoxLayout>
#include <QMouseEvent>

TitleBar::TitleBar(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(32);
    setObjectName("TitleBar");

    // 标题文本
    title_label_ = new QLabel(title, this);
    title_label_->setObjectName("TitleLabel");

    // 最小化按钮
    min_btn_ = new QPushButton("—", this);
    min_btn_->setObjectName("TitleBtnMin");
    min_btn_->setFixedSize(32, 32);
    QObject::connect(min_btn_, &QPushButton::clicked, this, &TitleBar::SigMinClicked);

    // 最大化/还原按钮
    max_btn_ = new QPushButton("□", this);
    max_btn_->setObjectName("TitleBtnMax");
    max_btn_->setFixedSize(32, 32);
    QObject::connect(max_btn_, &QPushButton::clicked, this, &TitleBar::SigMaxClicked);

    // 关闭按钮
    close_btn_ = new QPushButton("✕", this);
    close_btn_->setObjectName("TitleBtnClose");
    close_btn_->setFixedSize(32, 32);
    QObject::connect(close_btn_, &QPushButton::clicked, this, &TitleBar::SigCloseClicked);

    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(title_label_, 1);
    layout->addWidget(min_btn_);
    layout->addWidget(max_btn_);
    layout->addWidget(close_btn_);
}

void TitleBar::SetTitle(const QString& title)
{
    if (title_label_)
        title_label_->setText(title);
}

void TitleBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        drag_start_pos_ = event->globalPos() - window()->pos();   // 记录鼠标相对窗口左上角的偏移
}

void TitleBar::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton)
    {
        QWidget* win = window();
        win->move(event->globalPos() - drag_start_pos_);           // 新位置 = 鼠标全局位置 - 初始偏移
        // 不更新 drag_start_pos_，保持初始偏移量不变
    }
}
