#ifndef SERVERPANEL_H
#define SERVERPANEL_H

#include <QWidget>
#include <QLabel>
#include <QSpinBox>
#include <QLineEdit>
#include <QRadioButton>
#include <QComboBox>
#include <QPushButton>
#include <QTimer>
#include <thread>
#include <atomic>
#include <cstdint>

class StreamServer;
class SignalingClient;

// 被控端控制面板
// UI 线程：显示配置 + 启动/停止 + 状态监控
// 工作线程：StreamServer::Run() 阻塞循环
class ServerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ServerPanel(QWidget* parent = nullptr);
    ~ServerPanel();

    void SetSignalingClient(SignalingClient* client);   // 第九阶段：接入信令服务器

signals:
    void SigBack();                     // 返回启动页

private slots:
    void OnStartStop();                 // 启动/停止服务
    void OnUpdateStatus();              // 定时器：刷新状态

private:
    void LoadSettings();                // 加载默认配置
    void SaveSettings();                // 保存当前配置

    // UI
    QLabel*    ip_label_;
    QLineEdit* dest_ip_edit_;       // 目标客户端 IP
    QSpinBox*  video_port_;
    QSpinBox*  ctrl_port_;
    QSpinBox*  audio_port_;
    QSpinBox*  bitrate_spin_;
    QSpinBox*  fps_spin_;
    QRadioButton* fast_radio_;
    QRadioButton* cpu_radio_;
    QComboBox* monitor_combo_;
    QPushButton* start_btn_;
    QPushButton* back_btn_;

    QLabel*    status_frame_rate_;
    QLabel*    status_bitrate_;
    QLabel*    status_uptime_;
    QLabel*    status_client_;
    QTimer*    status_timer_;

    // 核心对象
    StreamServer* server_{nullptr};
    SignalingClient* signaling_{nullptr};   // 第九阶段
    std::thread   server_thread_;
    bool          server_running_{false};

    // 上一次统计基线
    uint64_t last_frame_count_{0};
    int64_t  start_time_ms_{0};
};

#endif // SERVERPANEL_H
