#ifndef GLOBAL_H
#define GLOBAL_H

#include <functional>
#include <memory>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "singletion.h"

inline std::string GetThreadName()
{
#ifdef _WIN32
    PWSTR desc;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)))
    {
        std::wstring ws(desc);
        LocalFree(desc);
        return std::string(ws.begin(), ws.end());
    }
#else
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    if (name[0]) return std::string(name);
#endif
    return "unknown";
}

using DeferFunc = std::function<void()>;
class Defer
{
public:
    Defer(DeferFunc func) : m_func(func) {}
    ~Defer() { m_func(); }
private:
    DeferFunc m_func;
};

class GlobalConfig : public Singleton<GlobalConfig>
{
public:
    friend class Singleton<GlobalConfig>;

public:
    bool Init(const std::string& app_dir_path);

    // ---- 服务器配置 ----
    int GetTcpPort() const { return tcp_port_; }
    int GetHttpPort() const { return http_port_; }
    int GetWsPort() const { return ws_port_; }                     // WebSocket 端口
    int GetStunPort() const { return stun_port_; }                // STUN 端口
    int GetMaxConnections() const { return max_connections_; }
    int GetWorkerThreads() const { return worker_threads_; }
    std::string GetLogLevel() const { return log_level_; }

    std::string GetDbPath() const { return db_path_; }

    std::string GetConfigPath() const { return config_path_; }

private:
    GlobalConfig() = default;

    std::string config_path_;

    // 服务器配置
    int tcp_port_ = 8079;
    int http_port_ = 8080;
    int ws_port_ = 8081;
    int stun_port_ = 3478;
    int max_connections_ = 10000;
    int worker_threads_ = 4;

    // 日志
    std::string log_level_ = "info";

    // 数据库
    std::string db_path_ = "./signaling.db";

    // 微信支付
    std::string wx_notify_url_;
    std::string pay_api_url_;
    std::string wx_merchant_id_;
    std::string wx_app_id_;
    std::string mid_platform_token_url_;
};

#endif // GLOBAL_H