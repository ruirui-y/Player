#include "WsSessionManager.h"
#include "WsSession.h"
#include "Global/LogManager.h"
#include <boost/json.hpp>

void WsSessionManager::Register(const std::string& device_id, WsSession* session)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 同一设备已有连接，关闭旧连接
    auto it = sessions_.find(device_id);
    if (it != sessions_.end())
    {
        LOG_INFO("ws", "[WsSessionManager] 替换旧连接: {}", device_id);
        sessions_.erase(it);
    }

    sessions_[device_id] = session;
}

void WsSessionManager::Remove(const std::string& device_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(device_id);
}

void WsSessionManager::SendToDevice(const std::string& device_id, const std::string& json)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(device_id);
    if (it != sessions_.end() && it->second)
    {
        LOG_INFO("ws", "[WsSessionManager] ↓ 推送到 {} : {} bytes", device_id, json.size());
        it->second->Send(json);
    }
    else
    {
        LOG_WARN("ws", "[WsSessionManager] 设备不在线: {}", device_id);
    }
}

void WsSessionManager::Broadcast(const std::string& json)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, session] : sessions_)
    {
        if (session)
            session->Send(json);
    }
}

// ---- 全局通知函数 ----
void NotifyDeviceOnline(const std::string& device_id)
{
    boost::json::object msg;
    msg["type"]   = "device_online";
    msg["id"]     = device_id;
    // 广播给所有已连接的设备
    WsSessionManager::Get().Broadcast(boost::json::serialize(msg));
}

void NotifyDeviceOffline(const std::string& device_id)
{
    boost::json::object msg;
    msg["type"]   = "device_offline";
    msg["id"]     = device_id;
    WsSessionManager::Get().Broadcast(boost::json::serialize(msg));
}
