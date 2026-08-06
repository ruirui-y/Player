#ifndef SIGNALINGCLIENT_H
#define SIGNALINGCLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <functional>

class QWebSocket;
class QTimer;

// 信令客户端（HTTP API + WebSocket 推送）
// 使用 Qt 内置网络库，零新增依赖
class SignalingClient : public QObject
{
    Q_OBJECT

public:
    explicit SignalingClient(QObject* parent = nullptr);
    ~SignalingClient();

    // ---- 服务器连接 ----
    void SetServer(const QString& host, int http_port = 8080, int ws_port = 8081);

    // ---- 设备管理 ----
    void RegisterDevice(const QString& device_id, const QString& name, const QString& local_ip = "");
    void StartHeartbeat(const QString& device_id, int interval_ms = 30000);  // 每30s
    void StopHeartbeat();

    // ---- 设备发现 ----
    void QueryDevices();  // GET /api/devices → 回调 OnDeviceList

    // ---- 打洞信令 ----
    void SendOffer(const QString& from, const QString& to, const QString& sdp);
    void SendAnswer(const QString& from, const QString& to, const QString& sdp);
    void SendIce(const QString& from, const QString& to, const QString& candidate);

    // ---- WebSocket ----
    void ConnectWebSocket(const QString& device_id);
    void DisconnectWebSocket();

    // ---- 回调 ----
    std::function<void(const QJsonArray& devices)> OnDeviceList;       // 设备列表
    std::function<void(const QJsonObject& signal)> OnSignaling;        // offer/answer/ice
    std::function<void(const QString& id, bool online)> OnDeviceStatus; // 上线/下线

signals:
    void SigConnected();
    void SigError(const QString& msg);

private slots:
    void OnWsConnected();
    void OnWsDisconnected();
    void OnWsTextMessage(const QString& msg);

private:
    void HttpPost(const QString& path, const QJsonObject& body,
                  std::function<void(const QJsonObject&)> callback = nullptr);
    void HttpGet(const QString& path,
                 std::function<void(const QJsonObject&)> callback);

    QNetworkAccessManager* http_;
    QWebSocket* ws_{ nullptr };
    QTimer* heartbeat_timer_{ nullptr };

    QString host_;
    int http_port_{ 8080 };
    int ws_port_{ 8081 };
    QString device_id_;
};

#endif // SIGNALINGCLIENT_H
