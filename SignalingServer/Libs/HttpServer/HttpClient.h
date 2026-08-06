#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <memory>
#include <string>

namespace httplib
{
    class Client;
    class Result;
}

class HttpClient
{
public:
    explicit HttpClient(const std::string& base_url);                               // 构造，设置基础 URL
    ~HttpClient();

    // ---- 请求头设置 ----
    void SetHeader(const std::string& key, const std::string& value);

    // ---- 阻塞请求 ----
    std::shared_ptr<httplib::Result> Get(const std::string& path);                  // GET 请求
    std::shared_ptr<httplib::Result> Post(const std::string& path,                  // POST 请求
        const std::string& body,
        const std::string& content_type = "application/json");

    // ---- 超时设置 ----
    void SetTimeout(int seconds);                                                   // 设置超时时间（秒）

private:
    std::unique_ptr<httplib::Client> client_;
};

#endif // HTTPCLIENT_H