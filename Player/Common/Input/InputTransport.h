#ifndef INPUTTRANSPORT_H
#define INPUTTRANSPORT_H

#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <WinSock2.h>
#include "InputEvent.h"

// ========== 控制消息协议 ==========

// 控制消息标记（区别于固定大小的 InputMessage）
static constexpr uint8_t CONTROL_MSG_MARKER = 0xFF;

// 控制消息包格式：
//   1 byte  Marker (0xFF)
//   1 byte  MsgType (ControlMsgType 枚举值)
//   2 bytes PayloadLen (大端序)
//   N bytes Payload（JSON 字符串或二进制数据）
//
// 输入事件格式（不加 Marker，直接发）：
//   12 bytes InputMessage（固定大小，第一个字节永远不是 0xFF）

// ========== 客户端：TCP 连接 + 发送 + 接收服务端消息 ==========

class InputTransportClient
{
public:
    InputTransportClient();                                                                 // 构造
    ~InputTransportClient();                                                                // 析构

    // 连接到服务端
    bool Connect(const char* server_ip, uint16_t port);                                     // 连接

    void Disconnect();                                                                      // 断开

    // 发送一条输入消息（非阻塞，失败返回 false）
    bool Send(const InputMessage& msg);                                                     // 发送

    // 发送一条控制消息（二进制协议：type + payload）
    // 兼容：SendControlMessage("request_idr") 等价于 SendControlMessage(1, "", 0)
    bool SendControlMessage(uint8_t msg_type, const uint8_t* payload = nullptr,
                            int payload_len = 0);                                           // 发送控制消息

    // 兼容旧接口：字符串 → 自动转为 type=0x01 + JSON
    bool SendControlMessage(const char* msg_type_str);                                      // 兼容旧 API

    bool IsConnected() const { return sock_ != INVALID_SOCKET; }                            // 是否已连接

    // 获取服务端发送的显示器信息（Connect 成功后调用）
    const ServerMonitorInfo& GetMonitorInfo() const { return monitor_info_; }               // 获取显示器信息

    // 设置服务端消息处理回调（接收线程中调用，需自行保证线程安全）
    void SetMessageHandler(std::function<void(uint8_t msg_type,
                            const uint8_t* payload, int payload_len)> handler);

private:
    SOCKET sock_{INVALID_SOCKET};                                                           // TCP socket
    bool wsa_started_{false};                                                               // WSA 是否已启动
    ServerMonitorInfo monitor_info_{};                                                      // 服务端显示器信息

    // 服务端消息接收
    std::function<void(uint8_t, const uint8_t*, int)> msg_handler_;                         // 消息回调
    std::thread recv_thread_;                                                               // 接收线程
    std::atomic<bool> recv_running_{false};                                                 // 接收线程运行标志
    void ClientReceiveLoop();                                                               // 客户端接收线程
};

// ========== 服务端：TCP 监听 + 接收输入事件 + 控制消息 + 发送通知 ==========

class InputTransportServer
{
public:
    InputTransportServer();                                                                 // 构造
    ~InputTransportServer();                                                                // 析构

    // 开始监听并启动接收线程
    bool Listen(uint16_t port);                                                             // 监听

    void Start();                                                                           // 启动接收线程
    void Stop();                                                                            // 停止

    // 设置客户端连接后发送的显示器信息（必须在 Start 之前调用）
    void SetMonitorInfo(const ServerMonitorInfo& info);                                     // 设置显示器信息

    // 收到输入事件时的回调（在接收线程中调用）
    std::function<void(const InputMessage&)> OnInputEvent;                                  // 事件回调

    // 收到控制消息时的回调（在接收线程中调用）
    // msg_type: ControlMsgType 枚举值
    // payload: 消息负载数据
    // payload_len: 负载长度
    std::function<void(uint8_t msg_type, const uint8_t* payload, int payload_len)>
        OnControlMessage;                                                                   // 控制消息回调（新版）

    // 兼容旧回调（如果新旧都设置，新回调优先）
    std::function<void(const char* msg_type)> OnControlMessageLegacy;                       // 旧版兼容

    // 发送消息到当前客户端（非阻塞，失败无影响）
    bool SendToClient(uint8_t msg_type, const uint8_t* payload, int payload_len);          // 发送消息

private:
    void AcceptLoop();                                                                      // 接受连接线程
    void ReceiveLoop(SOCKET client_sock);                                                   // 接收数据线程

    SOCKET listen_sock_{INVALID_SOCKET};                                                    // 监听 socket
    SOCKET client_sock_{INVALID_SOCKET};                                                    // 当前客户端 socket

    std::thread accept_thread_;                                                             // 接受连接 + 接收数据线程
    std::atomic<bool> running_{false};                                                      // 运行标志
    bool wsa_started_{false};                                                               // WSA 是否已启动

    ServerMonitorInfo monitor_info_{};                                                      // 发送给客户端的显示器信息
    bool has_monitor_info_{false};                                                          // 是否已设置显示器信息
};

#endif // INPUTTRANSPORT_H
