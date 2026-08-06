#include "HttpServerMgr.h"
#include "httplib.h"
#include "Global/LogManager.h"

HttpServerMgr::HttpServerMgr()
{
    server_ = std::make_unique<httplib::Server>();
}

HttpServerMgr::~HttpServerMgr()
{
    Stop();
}

void HttpServerMgr::Get(const std::string& path, HttpHandler handler)
{
    server_->Get(path, std::move(handler));
}

void HttpServerMgr::Post(const std::string& path, HttpHandler handler)
{
    server_->Post(path, std::move(handler));
}

void HttpServerMgr::Mount(const std::string& mount_point, const std::string& dir)
{
    server_->set_mount_point(mount_point, dir);
}

void HttpServerMgr::Start(int port)
{
    if (running_.exchange(true)) return;

    LOG_INFO("http", "[HttpServerMgr] 开始监听，端口:{}", port);

    server_->listen("0.0.0.0", port);

    LOG_INFO("http", "[HttpServerMgr] 监听已退出");
}

void HttpServerMgr::Stop()
{
    if (!running_.exchange(false)) return;

    server_->stop();
}