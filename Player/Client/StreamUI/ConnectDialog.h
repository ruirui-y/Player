#ifndef CONNECTDIALOG_H
#define CONNECTDIALOG_H

#include <QWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <cstdint>

class SignalingClient;
class QJsonArray;

// 控制端连接对话框
// 输入被控端 IP + 端口 + 帧率，支持最近连接列表
class ConnectDialog : public QWidget
{
    Q_OBJECT

public:
    explicit ConnectDialog(QWidget* parent = nullptr);
    void SetSignalingClient(SignalingClient* client);       // 第九阶段

signals:
    // 用户点击"连接"
    void SigConnect(uint16_t port, uint16_t ctrl_port, int fps);
    // 用户点击"返回"
    void SigBack();

private slots:
    void OnConnectClicked();
    void OnRecentClicked(QListWidgetItem* item);                                // 双击最近连接
    void OnRecentContextMenu(const QPoint& pos);                                // 右键菜单

private:
    void LoadRecent();                                                          // 加载最近连接
    void SaveRecent(const QString& data, const QString& display);               // 保存 data + 显示名

    QSpinBox*    port_spin_;
    QSpinBox*    ctrl_spin_;
    QSpinBox*    fps_spin_;
    QListWidget* recent_list_;
    QPushButton* connect_btn_;
    QPushButton* back_btn_;
    SignalingClient* signaling_{nullptr};   // 第九阶段
};

#endif // CONNECTDIALOG_H
