#include "LaunchPage.h"
#include <QVBoxLayout>

LaunchPage::LaunchPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LaunchPage");

    auto* layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(20);
    layout->setContentsMargins(60, 80, 60, 80);

    auto* title = new QLabel("Player", this);
    title->setObjectName("LaunchTitle");
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    btn_player_ = CreateModeButton("🎬", "播放视频", "本地视频/音频文件播放");
    layout->addWidget(btn_player_);
    btn_controller_ = CreateModeButton("🖥️", "远程控制", "连接并控制远程电脑");
    layout->addWidget(btn_controller_);
    btn_server_ = CreateModeButton("📡", "被控端", "允许其他设备控制本机");
    layout->addWidget(btn_server_);

    layout->addStretch();

    version_label_ = new QLabel("v1.0", this);
    version_label_->setObjectName("LaunchVersion");
    version_label_->setAlignment(Qt::AlignCenter);
    layout->addWidget(version_label_);

    QObject::connect(btn_player_, &QPushButton::clicked, this, &LaunchPage::SigPlayerMode);
    QObject::connect(btn_controller_, &QPushButton::clicked, this, &LaunchPage::SigControllerMode);
    QObject::connect(btn_server_, &QPushButton::clicked, this, &LaunchPage::SigServerMode);
}

QPushButton* LaunchPage::CreateModeButton(const QString& emoji, const QString& title,
                                           const QString& desc)
{
    auto* btn = new QPushButton(this);
    btn->setObjectName("LaunchBtn");
    btn->setText(QString("%1\n%2").arg(emoji).arg(title));
    btn->setMinimumHeight(80);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(desc);
    return btn;
}
