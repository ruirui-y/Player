#include "ConnectDialog.h"
#include "Client/Core/SignalingClient.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QSettings>
#include <QJsonArray>
#include <QMenu>
#include <QDebug>
#include <QSysInfo>
#include <QColor>

ConnectDialog::ConnectDialog(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ConnectDialog");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(50, 50, 50, 50);
    layout->setSpacing(16);

    auto* title = new QLabel("远程控制 — 连接被控端", this);
    title->setObjectName("ConnectTitle");
    layout->addWidget(title);

    auto* form = new QFormLayout();
    form->setSpacing(14);

    auto makeLabel = [this](const QString& text) -> QLabel* {
        auto* lb = new QLabel(text, this);
        lb->setObjectName("FormLabel");
        return lb;
    };

    port_spin_ = new QSpinBox(this);
    port_spin_->setObjectName("ConnectSpin");
    port_spin_->setRange(1024, 65535);
    port_spin_->setValue(47998);
    form->addRow(makeLabel("视频端口:"), port_spin_);

    ctrl_spin_ = new QSpinBox(this);
    ctrl_spin_->setObjectName("ConnectSpin");
    ctrl_spin_->setRange(1024, 65535);
    ctrl_spin_->setValue(47989);
    form->addRow(makeLabel("控制端口:"), ctrl_spin_);

    fps_spin_ = new QSpinBox(this);
    fps_spin_->setObjectName("ConnectSpin");
    fps_spin_->setRange(10, 240);
    fps_spin_->setValue(60);
    form->addRow(makeLabel("帧率:"), fps_spin_);

    layout->addLayout(form);

    auto* recent_label = new QLabel("最近连接:", this);
    recent_label->setObjectName("ConnectRecentLabel");
    layout->addWidget(recent_label);

    recent_list_ = new QListWidget(this);
    recent_list_->setObjectName("ConnectRecentList");
    recent_list_->setMaximumHeight(160);
    recent_list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(recent_list_);
    QObject::connect(recent_list_, &QListWidget::itemDoubleClicked,
                     this, &ConnectDialog::OnRecentClicked);
    QObject::connect(recent_list_, &QListWidget::customContextMenuRequested,
                     this, &ConnectDialog::OnRecentContextMenu);

    layout->addStretch();

    auto* btn_layout = new QHBoxLayout();
    back_btn_ = new QPushButton("返回", this);
    back_btn_->setObjectName("ConnectBackBtn");
    btn_layout->addWidget(back_btn_);

    connect_btn_ = new QPushButton("连接", this);
    connect_btn_->setObjectName("ConnectBtn");
    btn_layout->addWidget(connect_btn_);
    layout->addLayout(btn_layout);

    QObject::connect(back_btn_, &QPushButton::clicked, this, &ConnectDialog::SigBack);
    QObject::connect(connect_btn_, &QPushButton::clicked, this, &ConnectDialog::OnConnectClicked);

    LoadRecent();

    // 自动填充最近连接的第一条
    if (recent_list_->count() > 0)
    {
        QStringList parts = recent_list_->item(0)->data(Qt::UserRole).toString().split(':');
        if (parts.size() >= 2)
        {
            port_spin_->setValue(parts[0].toInt());
            if (parts.size() >= 2) ctrl_spin_->setValue(parts[1].toInt());
            if (parts.size() >= 3) fps_spin_->setValue(parts[2].toInt());
        }
    }
}

void ConnectDialog::OnConnectClicked()
{
    uint16_t port = static_cast<uint16_t>(port_spin_->value());
    uint16_t ctrl_port = static_cast<uint16_t>(ctrl_spin_->value());
    int fps = fps_spin_->value();

    // data: 纯冒号分隔 "ip:port:ctrl:fps"  display: 可读格式
    QString data    = QString("%1:%2:%3").arg(port).arg(ctrl_port).arg(fps);
    QString display = QString("%1 @ %2fps").arg(port).arg(fps);
    SaveRecent(data, display);

    emit SigConnect(port, ctrl_port, fps);
}

void ConnectDialog::OnRecentClicked(QListWidgetItem* item)
{
    // 从 Qt::UserRole 读数据 "ip:port:ctrl:fps"，纯冒号分隔，无歧义
    QStringList parts = item->data(Qt::UserRole).toString().split(':');
    if (parts.size() >= 2)
    {
        port_spin_->setValue(parts[0].toInt());
        if (parts.size() >= 2) ctrl_spin_->setValue(parts[1].toInt());
        if (parts.size() >= 3) fps_spin_->setValue(parts[2].toInt());
    }
}

void ConnectDialog::LoadRecent()
{
    QSettings settings("Player", "RemoteControl");
    QStringList list = settings.value("recent_connections").toStringList();
    bool migrated = false;                                  // 是否需要回写清理后的数据

    for (int i = 0; i < list.size(); ++i)
    {
        QString data = list[i];

        // ---- 迁移旧格式 ----
        // 旧格式: "ip:port:ctrl:120@120fps" → 新格式: "ip:port:ctrl:120"
        if (data.contains('@'))
        {
            QStringList parts = data.split(':');
            if (parts.size() >= 4)
            {
                // parts[3] = "120@120fps" → 提取 '@' 前的纯数字
                int at_pos = parts[3].indexOf('@');
                if (at_pos > 0)
                    parts[3] = parts[3].left(at_pos);       // "120"
            }
            data = parts.join(':');                         // 重组为纯冒号分隔
            list[i] = data;
            migrated = true;
        }

        // ---- 生成显示文本 ----
        QStringList parts = data.split(':');
        QString display;
        if (parts.size() >= 3)
            display = QString("%1:%2 @ %3fps").arg(parts[0]).arg(parts[1]).arg(parts[2]);
        else if (parts.size() >= 2)
            display = QString("%1:%2").arg(parts[0]).arg(parts[1]);
        else
            display = data;

        auto* item = new QListWidgetItem(display);
        item->setData(Qt::UserRole, data);                  // 存纯数字格式 "ip:port:ctrl:fps"
        recent_list_->addItem(item);
    }

    // 回写清理后的数据
    if (migrated)
        settings.setValue("recent_connections", list);
}

void ConnectDialog::SaveRecent(const QString& data, const QString& display)
{
    Q_UNUSED(display);

    QSettings settings("Player", "RemoteControl");
    QStringList list = settings.value("recent_connections").toStringList();
    list.removeAll(data);               // 去重
    list.prepend(data);                 // 最新排第一
    while (list.size() > 10) list.removeLast();
    settings.setValue("recent_connections", list);

    recent_list_->clear();
    for (const QString& d : list)
    {
        QStringList parts = d.split(':');
        QString disp;
        if (parts.size() >= 3)
            disp = QString("%1:%2 @ %3fps").arg(parts[0]).arg(parts[1]).arg(parts[2]);
        else if (parts.size() >= 2)
            disp = QString("%1:%2").arg(parts[0]).arg(parts[1]);
        else
            disp = d;

        auto* item = new QListWidgetItem(disp);
        item->setData(Qt::UserRole, d);
        recent_list_->addItem(item);
    }
}

void ConnectDialog::OnRecentContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = recent_list_->itemAt(pos);
    if (!item) return;

    QMenu menu(this);
    QAction* del = menu.addAction("删除");
    if (menu.exec(recent_list_->viewport()->mapToGlobal(pos)) == del)
    {
        QString data = item->data(Qt::UserRole).toString();
        delete recent_list_->takeItem(recent_list_->row(item));

        QSettings settings("Player", "RemoteControl");
        QStringList list = settings.value("recent_connections").toStringList();
        list.removeAll(data);
        settings.setValue("recent_connections", list);
    }
}

void ConnectDialog::SetSignalingClient(SignalingClient* client)
{
    signaling_ = client;
    if (!client) return;

    // 打开页面时自动查询设备列表 → 显示在最近连接区域
    QString my_id = QSysInfo::machineHostName();

    client->OnDeviceList = [this, my_id](const QJsonArray& devices) {
        qDebug() << "[ConnectDialog] 设备列表刷新:" << devices.size() << "个设备";
        recent_list_->clear();
        for (const auto& d : devices)
        {
            auto obj = d.toObject();
            bool online = obj["online"].toBool();
            QString name = obj["name"].toString();
            QString id   = obj["id"].toString();
            bool is_me   = (id == my_id);

            QString marker = is_me ? "★" : (online ? "●" : "○");
            QString text   = is_me
                ? QString(u8"%1 %2 (本机)").arg(marker).arg(name)
                : QString(u8"%1 %2").arg(marker).arg(name);
            qDebug() << "[ConnectDialog] 设备:" << text << id << online;

            auto* item = new QListWidgetItem(text);
            item->setData(Qt::UserRole, id);
            if (is_me)
                item->setForeground(QColor(255, 200, 50));   // 金色高亮本机
            else if (!online)
                item->setForeground(QColor(100, 100, 100));  // 灰色离线
            recent_list_->addItem(item);
        }
    };
    client->QueryDevices();

    // 设备上线/离线时自动刷新
    client->OnDeviceStatus = [this](const QString& id, bool online) {
        qDebug() << "[ConnectDialog] 设备状态变化:" << id << (online ? "上线" : "离线");
        if (signaling_) signaling_->QueryDevices();
    };
}