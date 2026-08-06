#include "StunServer.h"
#include "Global/LogManager.h"
#include <cstring>

StunServer::StunServer(boost::asio::io_context& io)
    : io_(io)
    , socket_(io)
{
}

void StunServer::Start(uint16_t port)
{
    socket_.open(boost::asio::ip::udp::v4());
    socket_.set_option(boost::asio::ip::udp::socket::reuse_address(true));
    socket_.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port));

    running_ = true;
    DoReceive();

    LOG_INFO("stun", "[StunServer] STUN 服务器已启动, UDP :{}", port);
}

void StunServer::Stop()
{
    running_ = false;
    boost::system::error_code ec;
    socket_.close(ec);
    LOG_INFO("stun", "[StunServer] 已停止");
}

void StunServer::DoReceive()
{
    socket_.async_receive_from(
        boost::asio::buffer(recv_buf_, sizeof(recv_buf_)),
        remote_,
        [this](boost::system::error_code ec, size_t bytes_recv)
        {
            if (!ec && bytes_recv >= 20)
            {
                // 解析 STUN 头部
                uint16_t type = (recv_buf_[0] << 8) | recv_buf_[1];
                uint16_t len  = (recv_buf_[2] << 8) | recv_buf_[3];

                if (type == BINDING_REQUEST)
                {
                    uint8_t response[32];
                    size_t resp_len = 0;
                    BuildResponse(remote_, recv_buf_, bytes_recv, response, resp_len);

                    socket_.async_send_to(
                        boost::asio::buffer(response, resp_len),
                        remote_,
                        [this](boost::system::error_code, size_t) {});

                    std::string ip = remote_.address().to_string();
                    uint16_t port  = remote_.port();
                    LOG_INFO("stun", "[StunServer] Binding Response → {}:{}", ip, port);

                    if (OnClientRequest)
                        OnClientRequest(ip, port);
                }
            }

            if (running_)
                DoReceive();
        });
}

void StunServer::BuildResponse(const boost::asio::ip::udp::endpoint& from,
                                const uint8_t* data, size_t len,
                                uint8_t* out, size_t& out_len)
{
    (void)len;

    // ---- STUN 头部 (20 字节) ----
    out[0] = (BINDING_RESPONSE >> 8) & 0xFF;
    out[1] = BINDING_RESPONSE & 0xFF;
    out[2] = 0x00;                        // Length 高字节
    out[3] = 0x0C;                        // Length = 12 (1 attribute, 8 bytes data + 4 header)

    // 复制 Transaction ID (12 bytes: 4 magic cookie + 12 random)
    std::memcpy(out + 4, data + 4, 12);   // data[4..15] → out[4..15]

    // ---- MAGIC_COOKIE 标记使用 XOR-MAPPED-ADDRESS ----
    // 如果请求中第4-7字节是 RFC 5387 magic cookie 0x2112A442，使用 XOR-MAPPED-ADDRESS
    uint32_t cookie = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    bool use_xor = (cookie == 0x2112A442);

    uint16_t attr_type = use_xor ? ATTR_XOR_MAPPED_ADDRESS : ATTR_MAPPED_ADDRESS;

    // ---- XOR-MAPPED-ADDRESS 属性 (12 字节) ----
    size_t pos = 20;
    out[pos++] = (attr_type >> 8) & 0xFF;
    out[pos++] = attr_type & 0xFF;
    out[pos++] = 0x00;                    // attr length = 8
    out[pos++] = 0x08;
    out[pos++] = 0x00;                    // reserved
    out[pos++] = 0x01;                    // family = IPv4

    uint16_t port = from.port();
    uint32_t addr = from.address().to_v4().to_uint();

    if (use_xor)
    {
        // XOR-Port: port ^ (MAGIC_COOKIE >> 16)
        port ^= (uint16_t)(MAGIC_COOKIE >> 16);
        // XOR-Address: addr ^ MAGIC_COOKIE
        addr ^= 0x2112A42F;
    }

    out[pos++] = (port >> 8) & 0xFF;
    out[pos++] = port & 0xFF;
    out[pos++] = (addr >> 24) & 0xFF;
    out[pos++] = (addr >> 16) & 0xFF;
    out[pos++] = (addr >> 8) & 0xFF;
    out[pos++] = addr & 0xFF;

    out_len = pos;
}
