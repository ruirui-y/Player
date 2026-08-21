#include "WsSession.h"
#include "WsSessionManager.h"
#include "Thread/WorkerThread.h"
#include "DB/SqlExec.h"
#include "Global/LogManager.h"
#include <boost/json.hpp>

std::shared_ptr<WsSession> WsSession::Create(tcp::socket&& socket)
{
    return std::shared_ptr<WsSession>(new WsSession(std::move(socket)));
}

WsSession::WsSession(tcp::socket&& socket)
    : ws_(std::move(socket))
{
}

void WsSession::Start()
{
    DoAccept();
}

void WsSession::DoAccept()
{
    auto self = shared_from_this();
    ws_.async_accept([self](beast::error_code ec)
    {
        if (ec)
        {
            LOG_ERROR("ws", "[WsSession] 握手失败: {}", ec.message());
            return;
        }

        LOG_INFO("ws", "[WsSession] WebSocket 握手成功, 等待 hello");
        self->DoRead();
    });
}

void WsSession::DoRead()
{
    auto self = shared_from_this();
    ws_.async_read(read_buf_, [self](beast::error_code ec, size_t)
    {
        if (ec)
        {
            if (!self->device_id.empty())
            {
                LOG_INFO("ws", "[WsSession] 设备离线: {}", self->device_id);

                // 更新数据库状态为离线
                auto* sql = GetCurrentThreadSqlExec();
                if (sql)
                {
                    char update[128];
                    snprintf(update, sizeof(update),
                        "UPDATE devices SET status=0 WHERE id='%s';",
                        self->device_id.c_str());
                    sql->Execute(update);
                }

                WsSessionManager::Get().Remove(self->device_id);
                WsSessionManager::Get().Broadcast(
                    R"({"type":"device_offline","id":")" + self->device_id + R"("})");
            }
            return;
        }

        std::string msg = beast::buffers_to_string(self->read_buf_.data());
        self->read_buf_.consume(self->read_buf_.size());
        self->OnMessage(msg);
        self->DoRead();
    });
}

void WsSession::OnMessage(const std::string& msg)
{
    try
    {
        auto j = boost::json::parse(msg);
        auto obj = j.as_object();

        // 第一条消息必须是 hello
        if (device_id.empty() && obj.contains("type"))
        {
            auto type = std::string(obj["type"].as_string().data(),
                                     obj["type"].as_string().size());
            if (type == "hello" && obj.contains("device_id"))
            {
                device_id = std::string(obj["device_id"].as_string().data(),
                                        obj["device_id"].as_string().size());
                WsSessionManager::Get().Register(device_id, this);
                LOG_INFO("ws", "[WsSession] 设备上线: {}", device_id);

                // 更新数据库状态（WS可能比HTTP register先到）
                auto* sql = GetCurrentThreadSqlExec();
                if (sql)
                {
                    char update[256];
                    snprintf(update, sizeof(update),
                        "UPDATE devices SET status=1, last_seen=strftime('%%s','now') WHERE id='%s';",
                        device_id.c_str());
                    sql->Execute(update);
                }

                // 广播上线通知
                WsSessionManager::Get().Broadcast(
                    R"({"type":"device_online","id":")" + device_id + R"("})");
                return;
            }
        }

        LOG_DEBUG("ws", "[WsSession] 收到消息: {} bytes", msg.size());
    }
    catch (...)
    {
        LOG_WARN("ws", "[WsSession] JSON 解析失败");
    }
}

void WsSession::Send(const std::string& msg)
{
    auto self = shared_from_this();
    ws_.async_write(boost::asio::buffer(msg), [self](beast::error_code ec, size_t)
    {
        if (ec)
            LOG_WARN("ws", "[WsSession] 发送失败: {}", ec.message());
    });
}
