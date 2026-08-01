#include "InputTransport.h"

#include <WS2tcpip.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <QDebug>

#pragma comment(lib, "ws2_32.lib")

// ================================================================
// ============== 客户端：TCP 连接 + 发送 + 接收 ==============
// ================================================================

InputTransportClient::InputTransportClient()
{
}

InputTransportClient::~InputTransportClient()
{
    Disconnect();
}

// 连接到服务端
bool InputTransportClient::Connect(const char* server_ip, uint16_t port)
{
    // ---- 第一步：初始化 Winsock ----
    WSADATA wsa_data;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0)
    {
        qDebug() << "[InputClient] WSAStartup 失败:" << err;
        return false;
    }
    wsa_started_ = true;

    // ---- 第二步：创建 TCP socket ----
    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET)
    {
        qDebug() << "[InputClient] socket 创建失败:" << WSAGetLastError();
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    // ---- 第三步：禁用 Nagle 算法 ----
    int nodelay = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY,
               (const char*)&nodelay, sizeof(nodelay));

    // ---- 第四步：连接服务端 ----
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &addr.sin_addr);

    if (connect(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        qDebug() << "[InputClient] connect 失败:" << WSAGetLastError();
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    qDebug() << "[InputClient] 已连接到控制信道" << server_ip << ":" << port;

    // ---- 第五步：接收服务端发送的显示器信息 ----
    int ret = recv(sock_, (char*)&monitor_info_, sizeof(ServerMonitorInfo), MSG_WAITALL);
    if (ret != sizeof(ServerMonitorInfo))
    {
        qDebug() << "[InputClient] 接收显示器信息失败, ret =" << ret;
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    qDebug() << "[InputClient] 显示器信息: capture=%dx%d  offset=(%d,%d)  virtual=%dx%d",
        monitor_info_.capture_width, monitor_info_.capture_height,
        monitor_info_.monitor_x, monitor_info_.monitor_y,
        monitor_info_.virtual_width, monitor_info_.virtual_height;

    // ---- 第六阶段新增：启动服务端消息接收线程 ----
    if (msg_handler_)
    {
        recv_running_ = true;
        recv_thread_ = std::thread(&InputTransportClient::ClientReceiveLoop, this);
    }

    return true;
}

// 断开连接
void InputTransportClient::Disconnect()
{
    // 停止接收线程
    if (recv_running_)
    {
        recv_running_ = false;
        if (sock_ != INVALID_SOCKET)
            shutdown(sock_, SD_RECEIVE);                    // 让 recv 退出
        if (recv_thread_.joinable())
            recv_thread_.join();
    }

    if (sock_ != INVALID_SOCKET)
    {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (wsa_started_)
    {
        WSACleanup();
        wsa_started_ = false;
    }
}

// 发送一条输入消息
bool InputTransportClient::Send(const InputMessage& msg)
{
    if (sock_ == INVALID_SOCKET)
        return false;

    int ret = send(sock_, (const char*)&msg, sizeof(InputMessage), 0);
    return ret == sizeof(InputMessage);
}

// 发送控制消息（二进制协议：0xFF + 1字节type + 2字节len + payload）
bool InputTransportClient::SendControlMessage(uint8_t msg_type,
    const uint8_t* payload, int payload_len)
{
    if (sock_ == INVALID_SOCKET)
        return false;

    if (payload_len < 0 || payload_len > 1024)
        return false;

    uint8_t buffer[1032];                                   // 4 字节头 + 最大 1024 字节负载
    buffer[0] = CONTROL_MSG_MARKER;
    buffer[1] = msg_type;
    uint16_t len_be = htons(static_cast<uint16_t>(payload_len));
    std::memcpy(buffer + 2, &len_be, 2);
    if (payload && payload_len > 0)
        std::memcpy(buffer + 4, payload, payload_len);

    int ret = send(sock_, (const char*)buffer, 4 + payload_len, 0);
    return ret == 4 + payload_len;
}

// 兼容旧接口：字符串 → type=0x01 + JSON payload
bool InputTransportClient::SendControlMessage(const char* msg_type_str)
{
    char json[64];
    int json_len = snprintf(json, sizeof(json), R"({"type":"%s"})", msg_type_str);
    if (json_len <= 0 || json_len >= (int)sizeof(json))
        return false;

    return SendControlMessage(0x01, (const uint8_t*)json, json_len);
}

// 设置服务端消息处理回调
void InputTransportClient::SetMessageHandler(
    std::function<void(uint8_t, const uint8_t*, int)> handler)
{
    msg_handler_ = std::move(handler);
}

// 客户端接收线程：循环 recv 服务端发送的消息
void InputTransportClient::ClientReceiveLoop()
{
    uint8_t recv_buf[1032];

    while (recv_running_)
    {
        // ---- 先读 1 字节 ----
        uint8_t first_byte;
        int ret = recv(sock_, (char*)&first_byte, 1, MSG_WAITALL);
        if (ret <= 0)
            break;

        if (first_byte != CONTROL_MSG_MARKER)
            continue;                                       // 非控制消息，跳过

        // ---- 读 type + len ----
        uint8_t buf3[3];
        ret = recv(sock_, (char*)buf3, 3, MSG_WAITALL);
        if (ret != 3)
            break;

        uint8_t msg_type = buf3[0];
        uint16_t payload_len;
        std::memcpy(&payload_len, buf3 + 1, 2);
        payload_len = ntohs(payload_len);

        if (payload_len == 0 || payload_len > 1024)
            continue;

        // ---- 读 payload ----
        ret = recv(sock_, (char*)recv_buf, payload_len, MSG_WAITALL);
        if (ret != (int)payload_len)
            break;

        if (msg_handler_)
            msg_handler_(msg_type, recv_buf, payload_len);
    }

    qDebug() << "[InputClient] 接收线程退出";
}

// ================================================================
// ============== 服务端：TCP 监听 + 接收输入 + 控制消息 + 发送 ==============
// ================================================================

InputTransportServer::InputTransportServer()
{
}

InputTransportServer::~InputTransportServer()
{
    Stop();
}

// 开始监听
bool InputTransportServer::Listen(uint16_t port)
{
    WSADATA wsa_data;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0)
    {
        qDebug() << "[InputServer] WSAStartup 失败:" << err;
        return false;
    }
    wsa_started_ = true;

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET)
    {
        qDebug() << "[InputServer] socket 创建失败:" << WSAGetLastError();
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    int reuse = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listen_sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        qDebug() << "[InputServer] bind 失败:" << WSAGetLastError();
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    if (listen(listen_sock_, 1) == SOCKET_ERROR)
    {
        qDebug() << "[InputServer] listen 失败:" << WSAGetLastError();
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    qDebug() << "[InputServer] 监听端口" << port << "，等待客户端连接...";
    return true;
}

void InputTransportServer::Start()
{
    if (running_)
        return;

    running_ = true;
    accept_thread_ = std::thread(&InputTransportServer::AcceptLoop, this);
}

void InputTransportServer::SetMonitorInfo(const ServerMonitorInfo& info)
{
    monitor_info_ = info;
    has_monitor_info_ = true;
}

void InputTransportServer::Stop()
{
    if (!running_)
        return;

    running_ = false;

    if (listen_sock_ != INVALID_SOCKET)
    {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }

    if (client_sock_ != INVALID_SOCKET)
    {
        closesocket(client_sock_);
        client_sock_ = INVALID_SOCKET;
    }

    if (accept_thread_.joinable())
        accept_thread_.join();

    if (wsa_started_)
    {
        WSACleanup();
        wsa_started_ = false;
    }

    qDebug() << "[InputServer] 已停止";
}

// 发送消息到当前客户端
bool InputTransportServer::SendToClient(uint8_t msg_type, const uint8_t* payload, int payload_len)
{
    if (client_sock_ == INVALID_SOCKET)
        return false;

    if (payload_len < 0 || payload_len > 1024)
        return false;

    uint8_t buffer[1032];
    buffer[0] = CONTROL_MSG_MARKER;
    buffer[1] = msg_type;
    uint16_t len_be = htons(static_cast<uint16_t>(payload_len));
    std::memcpy(buffer + 2, &len_be, 2);
    if (payload && payload_len > 0)
        std::memcpy(buffer + 4, payload, payload_len);

    int ret = send(client_sock_, (const char*)buffer, 4 + payload_len, 0);
    return ret == 4 + payload_len;
}

// 接受连接线程
void InputTransportServer::AcceptLoop()
{
    qDebug() << "[InputServer] 等待客户端连接...";

    while (running_)
    {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET new_sock = accept(listen_sock_, (sockaddr*)&client_addr, &addr_len);

        if (!running_ || new_sock == INVALID_SOCKET)
            break;

        int nodelay = 1;
        setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY,
                   (const char*)&nodelay, sizeof(nodelay));

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        qDebug() << "[InputServer] 客户端已连接:" << ip_str;

        client_sock_ = new_sock;

        if (has_monitor_info_)
        {
            int ret = send(new_sock, (const char*)&monitor_info_, sizeof(ServerMonitorInfo), 0);
            if (ret != sizeof(ServerMonitorInfo))
            {
                qDebug() << "[InputServer] 发送显示器信息失败, ret =" << ret;
                closesocket(new_sock);
                client_sock_ = INVALID_SOCKET;
                continue;
            }
            qDebug() << "[InputServer] 已发送显示器信息: capture=%dx%d  offset=(%d,%d)  virtual=%dx%d",
                monitor_info_.capture_width, monitor_info_.capture_height,
                monitor_info_.monitor_x, monitor_info_.monitor_y,
                monitor_info_.virtual_width, monitor_info_.virtual_height;
        }

        ReceiveLoop(new_sock);

        qDebug() << "[InputServer] 等待下一个客户端连接...";
    }

    qDebug() << "[InputServer] 接受连接线程退出";
}

// 接收数据线程（新协议：0xFF + type + len + payload）
void InputTransportServer::ReceiveLoop(SOCKET client_sock)
{
    while (running_)
    {
        // ---- 先读 1 字节，判断消息类型 ----
        uint8_t first_byte;
        int ret = recv(client_sock, (char*)&first_byte, 1, MSG_WAITALL);

        if (ret <= 0)
        {
            qDebug() << "[InputServer] 客户端断开，ret =" << ret;
            break;
        }

        if (first_byte == CONTROL_MSG_MARKER)
        {
            // ---- 控制消息：读 1 字节 type + 2 字节 len + payload ----
            uint8_t buf3[3];
            ret = recv(client_sock, (char*)buf3, 3, MSG_WAITALL);
            if (ret != 3)
                break;

            uint8_t msg_type = buf3[0];
            uint16_t payload_len;
            std::memcpy(&payload_len, buf3 + 1, 2);
            payload_len = ntohs(payload_len);

            if (payload_len == 0 || payload_len > 1024)
                continue;

            uint8_t recv_payload[1024];
            ret = recv(client_sock, (char*)recv_payload, payload_len, MSG_WAITALL);
            if (ret != (int)payload_len)
                break;

            // ---- 分发：新回调优先 ----
            if (OnControlMessage)
            {
                OnControlMessage(msg_type, recv_payload, payload_len);
            }
            else if (OnControlMessageLegacy)
            {
                // 兼容旧回调：提取 JSON type 字段
                std::string json_str((char*)recv_payload, payload_len);
                auto type_pos = json_str.find("\"type\":\"");
                if (type_pos != std::string::npos)
                {
                    auto start = type_pos + 8;
                    auto end = json_str.find('"', start);
                    if (end != std::string::npos)
                    {
                        std::string type_str = json_str.substr(start, end - start);
                        qDebug() << "[InputServer] 收到控制消息(legacy):" << type_str.c_str();
                        OnControlMessageLegacy(type_str.c_str());
                    }
                }
            }
        }
        else
        {
            // ---- 输入事件 ----
            InputMessage msg{};
            std::memcpy(&msg, &first_byte, 1);

            ret = recv(client_sock, (char*)&msg + 1, sizeof(InputMessage) - 1, MSG_WAITALL);
            if (ret != sizeof(InputMessage) - 1)
                break;

            if (OnInputEvent)
                OnInputEvent(msg);
        }
    }

    if (client_sock != INVALID_SOCKET)
        closesocket(client_sock);

    if (client_sock_ == client_sock)
        client_sock_ = INVALID_SOCKET;

    qDebug() << "[InputServer] 接收线程退出";
}
