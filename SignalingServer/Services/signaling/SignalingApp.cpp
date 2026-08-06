#include "SignalingApp.h"
#include "DB/SqlExec.h"
#include "Global/Global.h"
#include "Global/LogManager.h"
#include "Global/MetricsMgr.h"
#include "HttpServer/HttpServerMgr.h"
#include "HttpServer/httplib.h"
#include "Thread/ThreadPool.h"
#include "StunServer.h"
#include "WsSession.h"
#include "WsSessionManager.h"
#include <boost/json.hpp>
#include <filesystem>

SignalingApp::~SignalingApp()
{
    Stop();
}

bool SignalingApp::Init()
{
    namespace fs = std::filesystem;
    std::string cwd = fs::current_path().string();

    // ---- Step 1: 加载配置 ----
    auto cfg = GlobalConfig::Instance();
    if (!cfg->Init(cwd))
    {
        // 日志还没初始化，直接 stderr
        std::cerr << "[SignalingApp] 配置加载失败" << std::endl;
        return false;
    }

    // ---- Step 2: 初始化日志 ----
    LogManager::Instance()->Init("logs", cfg->GetLogLevel());

    // ---- Step 3: 启动线程池（Worker 初始化时各自打开 SQLite + 建表）----
    ThreadPool::Instance()->Start(cfg->GetWorkerThreads());

    // ---- Step 4: HTTP 路由 ----
    http_ = std::make_unique<HttpServerMgr>();
    InitHttpRoutes();

    // ---- Step 5: STUN 服务器 ----
    stun_ = std::make_unique<StunServer>(io_ctx_);

    // ---- Step 6: WebSocket 监听 ----
    InitWsAcceptor();

    LOG_INFO("app", "[SignalingApp] 初始化完成");
    return true;
}

void SignalingApp::Start()
{
    running_ = true;

    auto cfg = GlobalConfig::Instance();

    // HTTP 独立线程
    http_thread_ = std::thread([this, cfg]()
    {
        http_->Start(cfg->GetHttpPort());
    });

    // STUN 在主 io_context 上异步运行
    stun_->Start(cfg->GetStunPort());

    LOG_INFO("app", "[SignalingApp] HTTP 服务已启动 :{}, STUN :{}",
             cfg->GetHttpPort(), cfg->GetStunPort());

    io_ctx_.run();
}

void SignalingApp::Stop()
{
    if (!running_) return;
    running_ = false;

    io_ctx_.stop();
    if (stun_) stun_->Stop();
    if (http_) http_->Stop();

    if (http_thread_.joinable())
        http_thread_.join();

    ThreadPool::Instance()->Stop();
    LogManager::Instance()->Shutdown();
}

void SignalingApp::InitHttpRoutes()
{
    // ---- 设备注册 ----
    http_->Post("/api/register", [](const httplib::Request& req, httplib::Response& res)
    {
        res.set_header("Content-Type", "application/json");

        auto body = boost::json::parse(req.body);
        auto obj = body.as_object();

        std::string device_id = obj.contains("id") ? std::string(obj["id"].as_string()) : "";
        std::string name      = obj.contains("name") ? std::string(obj["name"].as_string()) : "";
        std::string local_ip  = obj.contains("local_ip") ? std::string(obj["local_ip"].as_string()) : "";

        if (device_id.empty() || name.empty())
        {
            res.status = 400;
            res.set_content(R"({"error":"missing id or name"})", "application/json");
            return;
        }

        auto* sql = GetCurrentThreadSqlExec();
        if (!sql)
        {
            res.status = 500;
            res.set_content(R"({"error":"db_unavailable"})", "application/json");
            return;
        }

        char upsert[1024];
        snprintf(upsert, sizeof(upsert),
            "INSERT INTO devices(id, name, local_ip, status, last_seen) "
            "VALUES('%s','%s','%s',1,strftime('%%s','now')) "
            "ON CONFLICT(id) DO UPDATE SET "
            "name='%s', local_ip='%s', status=1, last_seen=strftime('%%s','now');",
            device_id.c_str(), name.c_str(), local_ip.c_str(),
            name.c_str(), local_ip.c_str());

        if (sql->Execute(upsert) >= 0)
        {
            MetricsMgr::Instance()->IncrementCounter("register_success_total", "注册成功总数", 1);
            res.set_content(R"({"ok":true})", "application/json");
        }
        else
        {
            res.status = 500;
            res.set_content(R"({"error":"db_error"})", "application/json");
        }
    });

    // ---- 心跳 ----
    http_->Post("/api/heartbeat", [](const httplib::Request& req, httplib::Response& res)
    {
        res.set_header("Content-Type", "application/json");

        auto body = boost::json::parse(req.body);
        std::string device_id = body.as_object().contains("id") ?
            std::string(body.as_object()["id"].as_string()) : "";

        if (device_id.empty())
        {
            res.status = 400;
            res.set_content(R"({"error":"missing id"})", "application/json");
            return;
        }

        auto* sql = GetCurrentThreadSqlExec();
        char update[256];
        snprintf(update, sizeof(update),
            "UPDATE devices SET status=1, last_seen=strftime('%%s','now') WHERE id='%s';",
            device_id.c_str());

        if (sql && sql->Execute(update) >= 0)
            res.set_content(R"({"ok":true})", "application/json");
        else
        {
            res.status = 500;
            res.set_content(R"({"error":"db_error"})", "application/json");
        }
    });

    // ---- 设备列表 ----
    http_->Get("/api/devices", [](const httplib::Request& req, httplib::Response& res)
    {
        (void)req;
        res.set_header("Content-Type", "application/json");

        auto* sql = GetCurrentThreadSqlExec();
        if (!sql)
        {
            res.status = 500;
            res.set_content(R"({"error":"db_unavailable"})", "application/json");
            return;
        }

        sql->Execute("UPDATE devices SET status=0 WHERE "
            "status=1 AND (strftime('%s','now') - last_seen) > 60;");

        auto rows = sql->Query("SELECT id, name, public_ip, local_ip, status, last_seen "
            "FROM devices ORDER BY last_seen DESC;");

        boost::json::array devices;
        for (auto& row : rows)
        {
            boost::json::object dev;
            dev["id"] = row["id"];
            dev["name"] = row["name"];
            dev["public_ip"] = row["public_ip"];
            dev["local_ip"] = row["local_ip"];
            dev["online"] = (row["status"] == "1");
            devices.push_back(dev);
        }

        boost::json::object resp;
        resp["devices"] = devices;
        resp["count"]   = static_cast<int64_t>(devices.size());

        res.set_content(boost::json::serialize(resp), "application/json");
    });

    // ---- 打洞信令：转发 JSON 到目标设备的 WebSocket ----
    auto relaySignaling = [](const httplib::Request& req, httplib::Response& res,
                              const std::string& msg_type)
    {
        res.set_header("Content-Type", "application/json");

        auto body = boost::json::parse(req.body);
        auto obj = body.as_object();

        std::string from = obj.contains("from") ? std::string(obj["from"].as_string().data(),
            obj["from"].as_string().size()) : "";
        std::string to = obj.contains("to") ? std::string(obj["to"].as_string().data(),
            obj["to"].as_string().size()) : "";

        if (from.empty() || to.empty())
        {
            res.status = 400;
            res.set_content(R"({"error":"missing from or to"})", "application/json");
            return;
        }

        // 将 JSON 原样转发给目标设备
        WsSessionManager::Get().SendToDevice(to, req.body);

        res.set_content(R"({"ok":true})", "application/json");
        MetricsMgr::Instance()->IncrementCounter("signaling_" + msg_type + "_total",
            "信令 " + msg_type + " 总数", 1);
    };

    http_->Post("/api/offer", [relaySignaling](const httplib::Request& req, httplib::Response& res)
        { relaySignaling(req, res, "offer"); });

    http_->Post("/api/answer", [relaySignaling](const httplib::Request& req, httplib::Response& res)
        { relaySignaling(req, res, "answer"); });

    http_->Post("/api/ice", [relaySignaling](const httplib::Request& req, httplib::Response& res)
        { relaySignaling(req, res, "ice"); });

    // ---- 指标 ----
    http_->Get("/api/metrics", [](const httplib::Request& req, httplib::Response& res)
    {
        (void)req;
        res.set_header("Content-Type", "application/json");
        res.set_content(MetricsMgr::Instance()->CollectToJson(), "application/json");
    });

    LOG_INFO("app", "[SignalingApp] HTTP 路由注册完成");
}

// ---- WebSocket 监听 ----
void SignalingApp::InitWsAcceptor()
{
    auto cfg = GlobalConfig::Instance();
    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), cfg->GetWsPort());
    ws_acceptor_.open(ep.protocol());
    ws_acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    ws_acceptor_.bind(ep);
    ws_acceptor_.listen();

    DoAcceptWs();
    LOG_INFO("app", "[SignalingApp] WebSocket 监听已启动 :{}", cfg->GetWsPort());
}

void SignalingApp::DoAcceptWs()
{
    ws_acceptor_.async_accept(io_ctx_, [this](boost::beast::error_code ec, boost::asio::ip::tcp::socket socket)
    {
        if (!ec)
        {
            auto session = WsSession::Create(std::move(socket));
            session->Start();
        }
        DoAcceptWs();
    });
}
