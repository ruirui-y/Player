#ifndef INPUTTRANSPORT_H
#define INPUTTRANSPORT_H

#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <WinSock2.h>
#include "InputEvent.h"

// ========== 客户端：TCP 连接 + 发送输入事件 + 控制消息 ==========

class InputTransportClient
{
public:
    InputTransportClient();                                                                 // 构造
    ~InputTransportClient();                                                                // 析构

    // 连接到服务端
    // server_ip：服务端 IP（如 "127.0.0.1"）
    // port：控制信道端口（如 47989）
    // 连接成功后自动接收服务端发送的 ServerMonitorInfo
    bool Connect(const char* server_ip, uint16_t port);                                     // 连接

    void Disconnect();                                                                      // 断开

    // 发送一条输入消息（非阻塞，失败返回 false）
    bool Send(const InputMessage& msg);                                                     // 发送

    // 发送一条 JSON 控制消息（如 IDR 请求等）
    // msg_type：消息类型标识，如 "request_idr"
    bool SendControlMessage(const char* msg_type);                                          // 发送控制消息

    bool IsConnected() const { return sock_ != INVALID_SOCKET; }                            // 是否已连接

    // 获取服务端发送的显示器信息（Connect 成功后调用）
    const ServerMonitorInfo& GetMonitorInfo() const { return monitor_info_; }               // 获取显示器信息

private:
    SOCKET sock_{INVALID_SOCKET};                                                           // TCP socket
    bool wsa_started_{false};                                                               // WSA 是否已启动
    ServerMonitorInfo monitor_info_{};                                                      // 服务端显示器信息
};

// 控制消息类型（TCP 信道上用的 JSON 消息）
// 格式：1 字节标记 0xFF + 2 字节 JSON 长度 + JSON 字符串
// 服务端收到 0xFF 开头的数据时，后续数据按控制消息解析，不走 InputMessage 路径
static constexpr uint8_t CONTROL_MSG_MARKER = 0xFF;                                        // 控制消息标记

// ========== 服务端：TCP 监听 + 接收输入事件 ==========

class InputTransportServer
{
public:
    InputTransportServer();                                                                 // 构造
    ~InputTransportServer();                                                                // 析构

    // 开始监听并启动接收线程
    // port：监听端口（如 47989）
    bool Listen(uint16_t port);                                                             // 监听

    void Start();                                                                           // 启动接收线程
    void Stop();                                                                            // 停止

    // 设置客户端连接后发送的显示器信息（必须在 Start 之前调用）
    void SetMonitorInfo(const ServerMonitorInfo& info);                                     // 设置显示器信息

    // 收到输入事件时的回调（在接收线程中调用）
    std::function<void(const InputMessage&)> OnInputEvent;                                  // 事件回调

    // 收到控制消息时的回调（在接收线程中调用，传入 JSON 字符串）
    // 如收到 { "type": "request_idr" } 时，传入 "request_idr"
    std::function<void(const char* msg_type)> OnControlMessage;                             // 控制消息回调

private:
    void AcceptLoop();                                                                      // 接受连接线程
    void ReceiveLoop(SOCKET client_sock);                                                   // 接收数据线程

    SOCKET listen_sock_{INVALID_SOCKET};                                                    // 监听 socket
    SOCKET client_sock_{INVALID_SOCKET};                                                    // 当前客户端 socket

    std::thread accept_thread_;                                                             // 接受连接 + 接收数据线程（单线程循环）
    std::atomic<bool> running_{false};                                                      // 运行标志
    bool wsa_started_{false};                                                               // WSA 是否已启动

    ServerMonitorInfo monitor_info_{};                                                      // 发送给客户端的显示器信息
    bool has_monitor_info_{false};                                                          // 是否已设置显示器信息
};

#endif // INPUTTRANSPORT_H
