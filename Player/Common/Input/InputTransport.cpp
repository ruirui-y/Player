#include "InputTransport.h"

#include <WS2tcpip.h>
#include <QDebug>

#pragma comment(lib, "ws2_32.lib")

// ================================================================
// ============== 客户端：TCP 连接 + 发送 ==============
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

    // ---- 第三步：禁用 Nagle 算法（降低小包延迟） ----
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

    return true;
}

// 断开连接
void InputTransportClient::Disconnect()
{
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

// ================================================================
// ============== 服务端：TCP 监听 + 接收 ==============
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
    // ---- 第一步：初始化 Winsock ----
    WSADATA wsa_data;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0)
    {
        qDebug() << "[InputServer] WSAStartup 失败:" << err;
        return false;
    }
    wsa_started_ = true;

    // ---- 第二步：创建 TCP 监听 socket ----
    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET)
    {
        qDebug() << "[InputServer] socket 创建失败:" << WSAGetLastError();
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    // ---- 第三步：设置地址复用（避免 TIME_WAIT 导致 bind 失败） ----
    int reuse = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    // ---- 第四步：绑定端口 ----
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

    // ---- 第五步：开始监听 ----
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

// 启动接受连接线程
void InputTransportServer::Start()
{
    if (running_)
        return;

    running_ = true;
    accept_thread_ = std::thread(&InputTransportServer::AcceptLoop, this);
}

// 设置客户端连接后发送的显示器信息
void InputTransportServer::SetMonitorInfo(const ServerMonitorInfo& info)
{
    monitor_info_ = info;
    has_monitor_info_ = true;
}

// 停止
void InputTransportServer::Stop()
{
    if (!running_)
        return;

    running_ = false;

    // 关闭监听 socket 让 accept 退出
    if (listen_sock_ != INVALID_SOCKET)
    {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }

    // 关闭客户端 socket 让 recv 退出
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

// 接受连接线程（循环等待，客户端断开后重新 accept）
void InputTransportServer::AcceptLoop()
{
    qDebug() << "[InputServer] 等待客户端连接...";

    while (running_)
    {
        // ---- 阻塞等待客户端连接 ----
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET new_sock = accept(listen_sock_, (sockaddr*)&client_addr, &addr_len);

        if (!running_ || new_sock == INVALID_SOCKET)
            break;                                                      // 服务端停止或监听 socket 已关闭

        // ---- 禁用 Nagle 算法 ----
        int nodelay = 1;
        setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY,
                   (const char*)&nodelay, sizeof(nodelay));

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        qDebug() << "[InputServer] 客户端已连接:" << ip_str;

        client_sock_ = new_sock;

        // ---- 发送显示器信息（客户端需要此信息做正确的坐标映射） ----
        if (has_monitor_info_)
        {
            int ret = send(new_sock, (const char*)&monitor_info_, sizeof(ServerMonitorInfo), 0);
            if (ret != sizeof(ServerMonitorInfo))
            {
                qDebug() << "[InputServer] 发送显示器信息失败, ret =" << ret;
                closesocket(new_sock);
                client_sock_ = INVALID_SOCKET;
                continue;                                               // 关闭当前连接，继续等待下一个
            }
            qDebug() << "[InputServer] 已发送显示器信息: capture=%dx%d  offset=(%d,%d)  virtual=%dx%d",
                monitor_info_.capture_width, monitor_info_.capture_height,
                monitor_info_.monitor_x, monitor_info_.monitor_y,
                monitor_info_.virtual_width, monitor_info_.virtual_height;
        }

        // ---- 同步运行接收循环，直到客户端断开 ----
        // 不再启动新线程：AcceptLoop 线程直接执行 ReceiveLoop
        // recv 返回 <= 0 时退出 ReceiveLoop，回到 while 循环顶部重新 accept
        ReceiveLoop(new_sock);

        qDebug() << "[InputServer] 等待下一个客户端连接...";
    }

    qDebug() << "[InputServer] 接受连接线程退出";
}

// 接收数据线程
void InputTransportServer::ReceiveLoop(SOCKET client_sock)
{
    InputMessage msg{};

    while (running_)
    {
        // ---- 阻塞接收一条 InputMessage（固定 12 字节） ----
        int ret = recv(client_sock, (char*)&msg, sizeof(InputMessage), MSG_WAITALL);

        if (ret <= 0)
        {
            // 连接断开或出错
            qDebug() << "[InputServer] 客户端断开，ret =" << ret;
            break;
        }

        if (ret != sizeof(InputMessage))
        {
            // 不完整消息，丢弃
            continue;
        }

        // ---- 调用回调处理事件 ----
        if (OnInputEvent)
        {
            OnInputEvent(msg);
        }
    }

    if (client_sock != INVALID_SOCKET)
    {
        closesocket(client_sock);
    }

    if (client_sock_ == client_sock)
    {
        client_sock_ = INVALID_SOCKET;
    }

    qDebug() << "[InputServer] 接收线程退出";
}
