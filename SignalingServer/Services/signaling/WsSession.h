#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <string>
#include <memory>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = boost::asio::ip::tcp;

// WebSocket 会话：每个设备一个连接
// 生命周期管理: shared_ptr + enable_shared_from_this
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    static std::shared_ptr<WsSession> Create(tcp::socket&& socket);

    void Start();       // 开始 WebSocket 握手
    void Send(const std::string& msg);  // 发送 JSON 文本帧

    std::string device_id;  // 握手后从 URL 参数解析

private:
    explicit WsSession(tcp::socket&& socket);

    void DoAccept();    // WebSocket 升级握手
    void DoRead();      // 读取消息（保持长连接）
    void OnMessage(const std::string& msg);

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer read_buf_;
};
