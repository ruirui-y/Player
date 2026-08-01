#include "PlayerPage.h"
#include "VideoWidget.h"
#include "ControlBar.h"
#include "FileBrowser.h"
#include <QVBoxLayout>

PlayerPage::PlayerPage(QWidget* parent)
    : QWidget(parent)
{
    video_widget_ = new VideoWidget(this);
    control_bar_ = new ControlBar(this);
    file_browser_ = new FileBrowser(this);
    file_browser_->hide();                                      // 默认隐藏

    // ---- 左侧：视频区域 + 控制栏 ----
    QVBoxLayout* left_layout = new QVBoxLayout();
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(0);
    left_layout->addWidget(video_widget_, 1);
    left_layout->addWidget(control_bar_, 0);

    QWidget* left_container = new QWidget(this);
    left_container->setLayout(left_layout);

    // ---- QSplitter：左侧容器 + 文件浏览器 ----
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->addWidget(left_container);
    splitter_->addWidget(file_browser_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 0);
    splitter_->setHandleWidth(3);
    splitter_->setChildrenCollapsible(false);

    // ---- 主布局 ----
    QVBoxLayout* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);
    main_layout->addWidget(splitter_, 1);
}

void PlayerPage::ToggleFileBrowser()
{
    if (file_browser_)
        file_browser_->setVisible(!file_browser_->isVisible());
}
