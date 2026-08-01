// WinSock2 必须在 Qt 头文件之前包含
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "ServerPanel.h"
#include "Server/StreamServer.h"
#include "Server/OBS_Capture/MonitorCapture.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QSettings>
#include <QMessageBox>
#include <QDebug>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

ServerPanel::ServerPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ServerPanel");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(40, 40, 40, 40);
    layout->setSpacing(14);

    auto* title = new QLabel("被控端 — 等待远程连接", this);
    title->setObjectName("ServerTitle");
    layout->addWidget(title);

    // 本机 IP —— 遍历所有地址，跳过虚拟网卡/APIPA/回环，优先私有网段
    ip_label_ = new QLabel(this);
    ip_label_->setObjectName("ServerIpLabel");
    {
        char hostname[256] = {};
        gethostname(hostname, sizeof(hostname));
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        addrinfo* result = nullptr;

        QString best_ip;
        if (getaddrinfo(hostname, nullptr, &hints, &result) == 0)
        {
            for (addrinfo* ptr = result; ptr; ptr = ptr->ai_next)
            {
                sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(ptr->ai_addr);
                uint32_t ip_raw = ntohl(addr->sin_addr.s_addr);
                uint8_t b1 = (ip_raw >> 24) & 0xFF;
                uint8_t b2 = (ip_raw >> 16) & 0xFF;

                // 跳过无效地址
                if (b1 == 0)    continue;   // 0.x.x.x
                if (b1 == 127)  continue;   // 回环

                char ip_str[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
                QString ip = QString(ip_str);

                // APIPA (169.254.x.x) — 只作为最后备选
                if (b1 == 169 && b2 == 254)
                {
                    if (best_ip.isEmpty()) best_ip = ip;
                    continue;
                }

                // 私有网段 — 最优，立即返回
                if (b1 == 192 && b2 == 168) { best_ip = ip; break; }
                if (b1 == 10)                { best_ip = ip; break; }
                if (b1 == 172 && b2 >= 16 && b2 <= 31) { best_ip = ip; break; }

                // 公网 IP 或其他 — 次优选
                if (best_ip.isEmpty()) best_ip = ip;
            }
            freeaddrinfo(result);
        }

        if (!best_ip.isEmpty())
        {
            ip_label_->setText(QString("本机 IP: %1").arg(best_ip));
            ip_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 可选中复制
        }
        else
            ip_label_->setText("本机 IP: 未知");
    }
    layout->addWidget(ip_label_);

    // 目标客户端 IP（UDP 推流地址，默认本机测试）
    dest_ip_edit_ = new QLineEdit("127.0.0.1", this);
    dest_ip_edit_->setObjectName("ConnectIpEdit");
    dest_ip_edit_->setPlaceholderText("客户端 IP（如 192.168.1.50）");
    layout->addWidget(dest_ip_edit_);

    auto* form = new QFormLayout();
    form->setSpacing(12);

    auto makeLabel = [this](const QString& text) -> QLabel* {
        auto* lb = new QLabel(text, this);
        lb->setObjectName("FormLabel");
        return lb;
    };

    video_port_ = new QSpinBox(this); video_port_->setObjectName("ServerSpin");
    video_port_->setRange(1024, 65535); video_port_->setValue(47998);
    form->addRow(makeLabel("视频端口:"), video_port_);

    ctrl_port_ = new QSpinBox(this); ctrl_port_->setObjectName("ServerSpin");
    ctrl_port_->setRange(1024, 65535); ctrl_port_->setValue(47989);
    form->addRow(makeLabel("控制端口:"), ctrl_port_);

    audio_port_ = new QSpinBox(this); audio_port_->setObjectName("ServerSpin");
    audio_port_->setRange(1024, 65535); audio_port_->setValue(47997);
    form->addRow(makeLabel("音频端口:"), audio_port_);

    bitrate_spin_ = new QSpinBox(this); bitrate_spin_->setObjectName("ServerSpin");
    bitrate_spin_->setRange(1000, 100000); bitrate_spin_->setValue(10000);
    bitrate_spin_->setSingleStep(1000); bitrate_spin_->setSuffix(" kbps");
    form->addRow(makeLabel("码率:"), bitrate_spin_);

    fps_spin_ = new QSpinBox(this); fps_spin_->setObjectName("ServerSpin");
    fps_spin_->setRange(10, 240); fps_spin_->setValue(60); fps_spin_->setSuffix(" fps");
    form->addRow(makeLabel("帧率:"), fps_spin_);

    // 显示器选择
    monitor_combo_ = new QComboBox(this);
    monitor_combo_->setObjectName("ServerSpin");
    auto monitors = MonitorCapture::EnumerateMonitors();
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        int w = monitors[i].rect.right - monitors[i].rect.left;
        int h = monitors[i].rect.bottom - monitors[i].rect.top;
        monitor_combo_->addItem(
            QString("%1 (%2×%3)").arg(monitors[i].name).arg(w).arg(h), (int)i);
    }
    form->addRow(makeLabel("显示器:"), monitor_combo_);

    layout->addLayout(form);

    auto* enc_layout = new QHBoxLayout();
    fast_radio_ = new QRadioButton("快速 (GPU)", this); fast_radio_->setObjectName("ServerRadio");
    fast_radio_->setChecked(true);
    cpu_radio_ = new QRadioButton("兼容 (CPU)", this); cpu_radio_->setObjectName("ServerRadio");
    enc_layout->addWidget(new QLabel("编码器:", this));
    enc_layout->addWidget(fast_radio_); enc_layout->addWidget(cpu_radio_);
    enc_layout->addStretch();
    layout->addLayout(enc_layout);

    auto* status_group = new QGroupBox("运行状态", this);
    status_group->setObjectName("ServerStatusGroup");
    auto* status_layout = new QFormLayout(status_group);

    status_frame_rate_ = new QLabel("--", this); status_frame_rate_->setObjectName("ServerStatValue");
    status_layout->addRow("帧率:", status_frame_rate_);
    status_bitrate_ = new QLabel("--", this); status_bitrate_->setObjectName("ServerStatValue");
    status_layout->addRow("码率:", status_bitrate_);
    status_uptime_ = new QLabel("--", this); status_uptime_->setObjectName("ServerStatValue");
    status_layout->addRow("运行时长:", status_uptime_);
    status_client_ = new QLabel("等待连接...", this); status_client_->setObjectName("ServerClientStatus");
    status_layout->addRow("客户端:", status_client_);
    layout->addWidget(status_group);
    layout->addStretch();

    auto* btn_layout = new QHBoxLayout();
    back_btn_ = new QPushButton("返回", this); back_btn_->setObjectName("ServerBackBtn");
    btn_layout->addWidget(back_btn_);
    start_btn_ = new QPushButton("启动服务", this); start_btn_->setObjectName("ServerStartBtn");
    btn_layout->addWidget(start_btn_);
    layout->addLayout(btn_layout);

    status_timer_ = new QTimer(this);
    QObject::connect(status_timer_, &QTimer::timeout, this, &ServerPanel::OnUpdateStatus);
    QObject::connect(back_btn_, &QPushButton::clicked, this, [this]() {
        if (server_running_) OnStartStop();
        emit SigBack();
    });
    QObject::connect(start_btn_, &QPushButton::clicked, this, &ServerPanel::OnStartStop);

    LoadSettings();
}

ServerPanel::~ServerPanel()
{
    if (server_running_)
    {
        server_->Stop();
        if (server_thread_.joinable()) server_thread_.join();
        delete server_; server_ = nullptr;
    }
}

void ServerPanel::OnStartStop()
{
    if (server_running_)
    {
        server_->Stop();
        if (server_thread_.joinable()) server_thread_.join();
        delete server_; server_ = nullptr;
        server_running_ = false;
        status_timer_->stop();
        start_btn_->setText("启动服务");
        video_port_->setEnabled(true); ctrl_port_->setEnabled(true);
        audio_port_->setEnabled(true); bitrate_spin_->setEnabled(true);
        fps_spin_->setEnabled(true);
        dest_ip_edit_->setEnabled(true);
        fast_radio_->setEnabled(true); cpu_radio_->setEnabled(true);
        status_client_->setText("已停止");
    }
    else
    {
        SaveSettings();
        uint16_t vp = static_cast<uint16_t>(video_port_->value());
        uint16_t cp = static_cast<uint16_t>(ctrl_port_->value());
        uint16_t ap = static_cast<uint16_t>(audio_port_->value());
        int bitrate = bitrate_spin_->value(); int fps = fps_spin_->value();
        bool use_fast = fast_radio_->isChecked();

        server_ = new StreamServer();
        std::string dest_ip = dest_ip_edit_->text().trimmed().toStdString();
        if (!server_->Init(vp, monitor_combo_->currentData().toInt(),
                           dest_ip.c_str(), fps, bitrate, cp, use_fast, ap))
        {
            QMessageBox::warning(this, "错误", "服务启动失败");
            delete server_; server_ = nullptr; return;
        }

        video_port_->setEnabled(false); ctrl_port_->setEnabled(false);
        audio_port_->setEnabled(false); bitrate_spin_->setEnabled(false);
        fps_spin_->setEnabled(false);
        dest_ip_edit_->setEnabled(false);
        fast_radio_->setEnabled(false); cpu_radio_->setEnabled(false);
        server_running_ = true;
        start_btn_->setText("停止服务");

        last_frame_count_ = 0;
        start_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        status_timer_->start(500);

        server_thread_ = std::thread([this]() { server_->Run(); });
    }
}

void ServerPanel::OnUpdateStatus()
{
    if (!server_ || !server_running_) return;
    uint64_t frame_count = server_->frame_count_.load();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    double elapsed = (now_ms - start_time_ms_) / 1000.0;
    if (elapsed > 0.5)
    {
        double fps = (frame_count - last_frame_count_) / elapsed;
        status_frame_rate_->setText(QString::number(static_cast<int>(fps + 0.5)));
        last_frame_count_ = frame_count; start_time_ms_ = now_ms;
    }
    status_bitrate_->setText(QString("%1 kbps").arg(server_->current_bitrate_.load()));
    int total_sec = static_cast<int>(frame_count / fps_spin_->value());
    status_uptime_->setText(QString("%1:%2:%3")
        .arg(total_sec/3600,2,10,QChar('0')).arg((total_sec%3600)/60,2,10,QChar('0')).arg(total_sec%60,2,10,QChar('0')));
    if (server_->client_connected_.load())
        status_client_->setText("已连接");
    else
        status_client_->setText("等待连接...");
}

void ServerPanel::LoadSettings()
{
    QSettings settings("Player", "RemoteControl");
    video_port_->setValue(settings.value("server/video_port", 47998).toInt());
    ctrl_port_->setValue(settings.value("server/ctrl_port", 47989).toInt());
    audio_port_->setValue(settings.value("server/audio_port", 47997).toInt());
    bitrate_spin_->setValue(settings.value("server/bitrate", 10000).toInt());
    fps_spin_->setValue(settings.value("server/fps", 60).toInt());
    dest_ip_edit_->setText(settings.value("server/dest_ip", "127.0.0.1").toString());
    int monitor_idx = settings.value("server/monitor", 0).toInt();
    if (monitor_idx < monitor_combo_->count())
        monitor_combo_->setCurrentIndex(monitor_idx);
    bool f = settings.value("server/fast_encoder", true).toBool();
    fast_radio_->setChecked(f); cpu_radio_->setChecked(!f);
}

void ServerPanel::SaveSettings()
{
    QSettings settings("Player", "RemoteControl");
    settings.setValue("server/video_port", video_port_->value());
    settings.setValue("server/ctrl_port", ctrl_port_->value());
    settings.setValue("server/audio_port", audio_port_->value());
    settings.setValue("server/bitrate", bitrate_spin_->value());
    settings.setValue("server/fps", fps_spin_->value());
    settings.setValue("server/dest_ip", dest_ip_edit_->text().trimmed());
    settings.setValue("server/monitor", monitor_combo_->currentIndex());
    settings.setValue("server/fast_encoder", fast_radio_->isChecked());
}
