#ifndef WSSESSIONMANAGER_H
#define WSSESSIONMANAGER_H

#include <string>
#include <mutex>
#include <unordered_map>
#include <functional>

class WsSession;

// WebSocket 会话管理器（仿 DemandStation SessionManager 双映射模式）
// device_id → WsSession*, 单设备单连接
class WsSessionManager
{
public:
    static WsSessionManager& Get()
    {
        static WsSessionManager mgr;
        return mgr;
    }

    // 注册/移除会话
    void Register(const std::string& device_id, WsSession* session);
    void Remove(const std::string& device_id);

    // 向指定设备推送 JSON 消息
    void SendToDevice(const std::string& device_id, const std::string& json);

    // 广播给所有在线设备
    void Broadcast(const std::string& json);

    // 设备在线回调
    using StatusCallback = std::function<void(const std::string& device_id, bool online)>;
    void SetStatusCallback(StatusCallback cb) { on_status_change_ = std::move(cb); }

private:
    WsSessionManager() = default;

    std::mutex mutex_;
    std::unordered_map<std::string, WsSession*> sessions_;
    StatusCallback on_status_change_;
};

// 设备上线/下线通知
void NotifyDeviceOnline(const std::string& device_id);
void NotifyDeviceOffline(const std::string& device_id);

#endif // WSSESSIONMANAGER_H
