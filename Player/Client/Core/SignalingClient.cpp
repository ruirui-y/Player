#include "SignalingClient.h"
#include <QWebSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QDebug>

SignalingClient::SignalingClient(QObject* parent)
    : QObject(parent)
    , http_(new QNetworkAccessManager(this))
{
}

SignalingClient::~SignalingClient()
{
    StopHeartbeat();
    DisconnectWebSocket();
}

void SignalingClient::SetServer(const QString& host, int http_port, int ws_port)
{
    host_ = host;
    http_port_ = http_port;
    ws_port_ = ws_port;
}

// ---- HTTP 请求 ----
void SignalingClient::HttpPost(const QString& path, const QJsonObject& body,
                                std::function<void(const QJsonObject&)> callback)
{
    QUrl url(QString("http://%1:%2%3").arg(host_).arg(http_port_).arg(path));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = http_->post(req, data);

    connect(reply, &QNetworkReply::finished, this, [reply, callback, this]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            qDebug() << "[SignalingClient] HTTP error:" << reply->errorString();
            return;
        }
        if (callback)
        {
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            callback(doc.object());
        }
    });
}

void SignalingClient::HttpGet(const QString& path,
                               std::function<void(const QJsonObject&)> callback)
{
    QUrl url(QString("http://%1:%2%3").arg(host_).arg(http_port_).arg(path));
    QNetworkRequest req(url);

    QNetworkReply* reply = http_->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            qDebug() << "[SignalingClient] HTTP error:" << reply->errorString();
            return;
        }
        if (callback)
        {
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            callback(doc.object());
        }
    });
}

// ---- 设备注册 ----
void SignalingClient::RegisterDevice(const QString& device_id, const QString& name,
                                      const QString& local_ip)
{
    device_id_ = device_id;

    QJsonObject body;
    body["id"]  = device_id;
    body["name"] = name;
    body["local_ip"] = local_ip;

    HttpPost("/api/register", body);
    qDebug() << "[SignalingClient] Registered:" << device_id;

    // 同时连接 WebSocket 接收推送
    ConnectWebSocket(device_id);
}

// ---- 心跳 ----
void SignalingClient::StartHeartbeat(const QString& device_id, int interval_ms)
{
    if (!heartbeat_timer_)
        heartbeat_timer_ = new QTimer(this);

    connect(heartbeat_timer_, &QTimer::timeout, this, [this, device_id]() {
        QJsonObject body;
        body["id"] = device_id;
        HttpPost("/api/heartbeat", body);
    });

    heartbeat_timer_->start(interval_ms);
}

void SignalingClient::StopHeartbeat()
{
    if (heartbeat_timer_)
    {
        heartbeat_timer_->stop();
        disconnect(heartbeat_timer_, nullptr, this, nullptr);
    }
}

// ---- 设备列表 ----
void SignalingClient::QueryDevices()
{
    HttpGet("/api/devices", [this](const QJsonObject& resp) {
        if (OnDeviceList && resp.contains("devices"))
            OnDeviceList(resp["devices"].toArray());
    });
}

// ---- 打洞信令 ----
void SignalingClient::SendOffer(const QString& from, const QString& to, const QString& sdp)
{
    QJsonObject body;
    body["from"] = from;
    body["to"]   = to;
    body["sdp"]  = sdp;
    HttpPost("/api/offer", body);
}

void SignalingClient::SendAnswer(const QString& from, const QString& to, const QString& sdp)
{
    QJsonObject body;
    body["from"] = from;
    body["to"]   = to;
    body["sdp"]  = sdp;
    HttpPost("/api/answer", body);
}

void SignalingClient::SendIce(const QString& from, const QString& to, const QString& candidate)
{
    QJsonObject body;
    body["from"]      = from;
    body["to"]        = to;
    body["candidate"] = candidate;
    HttpPost("/api/ice", body);
}

// ---- WebSocket ----
void SignalingClient::ConnectWebSocket(const QString& device_id)
{
    if (ws_) return;

    ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    // 先注册所有信号，再 open（避免信号在注册前触发）
    connect(ws_, &QWebSocket::connected, this, &SignalingClient::OnWsConnected);
    connect(ws_, &QWebSocket::disconnected, this, &SignalingClient::OnWsDisconnected);
    connect(ws_, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, [](QAbstractSocket::SocketError e) {
        qDebug() << "[SignalingClient] WebSocket error:" << e;
    });
    connect(ws_, &QWebSocket::textMessageReceived, this, &SignalingClient::OnWsTextMessage);

    connect(ws_, &QWebSocket::connected, this, [this, device_id]() {
        QJsonObject hello;
        hello["type"] = "hello";
        hello["device_id"] = device_id;
        ws_->sendTextMessage(QJsonDocument(hello).toJson(QJsonDocument::Compact));
    });

    QString ws_url = QString("ws://%1:%2/ws").arg(host_).arg(ws_port_);
    ws_->open(QUrl(ws_url));
}

void SignalingClient::DisconnectWebSocket()
{
    if (ws_)
    {
        ws_->close();
        ws_->deleteLater();
        ws_ = nullptr;
    }
}

void SignalingClient::OnWsConnected()
{
    qDebug() << "[SignalingClient] WebSocket 已连接";
    emit SigConnected();
}

void SignalingClient::OnWsDisconnected()
{
    qDebug() << "[SignalingClient] WebSocket 断开";
}

void SignalingClient::OnWsTextMessage(const QString& msg)
{
    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
    if (!doc.isObject()) return;

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if (type == "device_online" || type == "device_offline")
    {
        if (OnDeviceStatus)
            OnDeviceStatus(obj["id"].toString(), type == "device_online");
    }
    else if (type == "offer" || type == "answer" || type == "ice")
    {
        if (OnSignaling)
            OnSignaling(obj);
    }
}
