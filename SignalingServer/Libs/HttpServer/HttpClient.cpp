#include "HttpClient.h"
#include "httplib.h"
#include "Global/LogManager.h"

HttpClient::HttpClient(const std::string& base_url)
{
    client_ = std::make_unique<httplib::Client>(base_url);

    // 默认超时：30 秒
    client_->set_connection_timeout(30);
    client_->set_read_timeout(30);
}

HttpClient::~HttpClient()
{
}

void HttpClient::SetHeader(const std::string& key, const std::string& value)
{
    client_->set_default_headers({ {key, value} });
}

void HttpClient::SetTimeout(int seconds)
{
    client_->set_connection_timeout(seconds);
    client_->set_read_timeout(seconds);
}

// ---- 阻塞 GET ----
std::shared_ptr<httplib::Result> HttpClient::Get(const std::string& path)
{
    try
    {
        auto res = std::make_shared<httplib::Result>(client_->Get(path));
        return res;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("http", "[HttpClient] GET 异常: {} {}", path, e.what());
        return nullptr;
    }
}

// ---- 阻塞 POST ----
std::shared_ptr<httplib::Result> HttpClient::Post(const std::string& path,
    const std::string& body, const std::string& content_type)
{
    try
    {
        auto res = std::make_shared<httplib::Result>(
            client_->Post(path, body, content_type));
        return res;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("http", "[HttpClient] POST 异常: {} {}", path, e.what());
        return nullptr;
    }
}