#ifndef SIGNALINGAPP_H
#define SIGNALINGAPP_H

#include <boost/asio.hpp>
#include <thread>
#include <atomic>
#include <memory>

#include "HttpServer/HttpServerMgr.h"
#include "StunServer.h"
#include <boost/asio/ip/tcp.hpp>

class WsSession;

// 信令服务器主应用
class SignalingApp
{
public:
    SignalingApp() = default;
    ~SignalingApp();

    bool Init();
    void Start();
    void Stop();

private:
    void InitHttpRoutes();
    void InitWsAcceptor();
    void DoAcceptWs();           // async_accept 递归

    boost::asio::io_context io_ctx_;
    boost::asio::ip::tcp::acceptor ws_acceptor_{io_ctx_};
    std::unique_ptr<HttpServerMgr> http_;
    std::unique_ptr<StunServer> stun_;
    std::thread http_thread_;
    std::atomic<bool> running_{ false };
};

#endif // SIGNALINGAPP_H
