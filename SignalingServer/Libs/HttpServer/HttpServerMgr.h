#ifndef HTTPSERVERMGR_H
#define HTTPSERVERMGR_H

#include <memory>
#include <string>
#include <functional>
#include <atomic>

namespace httplib
{
    class Server;
    class Request;
    class Response;
}

class HttpServerMgr
{
public:
    HttpServerMgr();
    ~HttpServerMgr();

    using HttpHandler = std::function<void(const httplib::Request&, httplib::Response&)>;

    // ---- 路由注册 ----
    void Get(const std::string& path, HttpHandler handler);
    void Post(const std::string& path, HttpHandler handler);

    // ---- 挂载静态目录 ----
    void Mount(const std::string& mount_point, const std::string& dir);

    // ---- 生命周期 ----
    void Start(int port);
    void Stop();

private:
    std::unique_ptr<httplib::Server> server_;
    std::atomic<bool> running_{ false };
};

#endif // HTTPSERVERMGR_H