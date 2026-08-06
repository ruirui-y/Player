#include "Global.h"
#include "JsonTool.h"
#include "LogManager.h"
#include <boost/json.hpp>
#include <filesystem>

bool GlobalConfig::Init(const std::string& app_dir_path)
{
    config_path_ = app_dir_path + "/Config/server_config.json";

    boost::json::value doc;
    std::string err_str;
    if (!JsonTool::Instance()->ReadJsonFile(config_path_, doc, &err_str))
    {
        LOG_ERROR("global", "[GlobalConfig] 读取配置文件失败: {}", err_str);
        return false;
    }

    boost::json::object root = doc.as_object();

    // ---- 解析 Server 节点 ----
    if (root.contains("Server"))
    {
        boost::json::object srv = root["Server"].as_object();
        http_port_       = srv.contains("HttpPort")       ? static_cast<int>(srv["HttpPort"].as_int64())       : 8080;
        ws_port_         = srv.contains("WsPort")         ? static_cast<int>(srv["WsPort"].as_int64())         : 8081;
        stun_port_       = srv.contains("StunPort")       ? static_cast<int>(srv["StunPort"].as_int64())       : 3478;
        max_connections_ = srv.contains("MaxConnections") ? static_cast<int>(srv["MaxConnections"].as_int64()) : 10000;
        worker_threads_  = srv.contains("WorkerThreads")  ? static_cast<int>(srv["WorkerThreads"].as_int64())  : 4;
        log_level_       = srv.contains("LogLevel")       ? std::string(srv["LogLevel"].as_string())           : "info";
    }

    // ---- 解析 Database 节点 ----
    if (root.contains("Database"))
    {
        boost::json::object db = root["Database"].as_object();
        db_path_ = db.contains("Path") ? std::string(db["Path"].as_string()) : "./signaling.db";
    }

    return true;
}
