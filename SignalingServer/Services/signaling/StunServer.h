#ifndef STUNSERVER_H
#define STUNSERVER_H

#include <boost/asio.hpp>
#include <functional>
#include <string>
#include <cstdint>

// RFC 5389 STUN 服务器
// 监听 UDP :3478，收到 Binding Request 后返回 MAPPED-ADDRESS
// 客户端从中获取自己的公网 IP:Port
class StunServer
{
public:
    explicit StunServer(boost::asio::io_context& io);

    void Start(uint16_t port);
    void Stop();

    // 回调: 客户端请求的源地址（ip, port）
    std::function<void(const std::string&, uint16_t)> OnClientRequest;

private:
    void DoReceive();
    void BuildResponse(const boost::asio::ip::udp::endpoint& from,
                       const uint8_t* data, size_t len,
                       uint8_t* out_buf, size_t& out_len);

    boost::asio::io_context& io_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint remote_;
    uint8_t recv_buf_[64];
    bool running_{ false };

    static constexpr uint16_t BINDING_REQUEST  = 0x0001;
    static constexpr uint16_t BINDING_RESPONSE = 0x0101;
    static constexpr uint16_t ATTR_MAPPED_ADDRESS = 0x0001;
    static constexpr uint16_t ATTR_XOR_MAPPED_ADDRESS = 0x0020;
    static constexpr uint16_t MAGIC_COOKIE = 0x2112;       // RFC 5387
};

#endif // STUNSERVER_H
