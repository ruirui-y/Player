#include "TitleBar.h"
#include <QHBoxLayout>
#include <QMouseEvent>

TitleBar::TitleBar(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);

    setFixedHeight(32);
    setObjectName("TitleBar");

    // 返回按钮（默认隐藏，非启动页时显示）
    back_btn_ = new QPushButton("⏴", this);
    back_btn_->setObjectName("TitleBtnBack");
    back_btn_->setFixedSize(32, 32);
    back_btn_->hide();
    QObject::connect(back_btn_, &QPushButton::clicked, this, &TitleBar::SigBackClicked);

    // 标题文本
    title_label_ = new QLabel(title, this);
    title_label_->setObjectName("TitleLabel");

    // 最小化按钮
    min_btn_ = new QPushButton("—", this);
    min_btn_->setObjectName("TitleBtnMin");
    min_btn_->setFixedSize(32, 32);
    QObject::connect(min_btn_, &QPushButton::clicked, this, &TitleBar::SigMinClicked);

    // 最大化/还原按钮
    max_btn_ = new QPushButton("🗖", this);
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
    layout->addWidget(back_btn_);
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

void TitleBar::ShowBackButton(bool show)
{
    if (back_btn_)
        back_btn_->setVisible(show);
}

void TitleBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        drag_start_pos_ = event->globalPos() - window()->pos();
}

void TitleBar::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton)
    {
        QWidget* win = window();
        win->move(event->globalPos() - drag_start_pos_);
    }
}