#ifndef SIGNALINGAPP_H
#define SIGNALINGAPP_H

#include <boost/asio.hpp>
#include <thread>
#include <atomic>
#include <memory>

#include "HttpServer/HttpServerMgr.h"
#include "StunServer.h"

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
    void InitHttpRoutes();       // 注册 HTTP API 路由

    boost::asio::io_context io_ctx_;
    std::unique_ptr<HttpServerMgr> http_;
    std::unique_ptr<StunServer> stun_;
    std::thread http_thread_;
    std::atomic<bool> running_{ false };
};

#endif // SIGNALINGAPP_H
