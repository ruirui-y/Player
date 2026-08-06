#include "WorkerThread.h"
#include "DB/SqlExec.h"
#include "Global/Global.h"
#include "Global/LogManager.h"

// ---- thread_local 资源指针 ----
static thread_local SqlExec* tls_sql_exec_ = nullptr;
static thread_local WorkerThread* tls_worker_ = nullptr;

// ---- 全局访问器 ----
SqlExec* GetCurrentThreadSqlExec()
{
    // Worker 线程：已有 SqlExec
    if (tls_sql_exec_)
        return tls_sql_exec_;

    // 非 Worker 线程（httplib HTTP 线程等）：按需创建
    auto* sql = new SqlExec();
    sql->Init(GlobalConfig::Instance()->GetDbPath());
    tls_sql_exec_ = sql;
    return sql;
}
WorkerThread* GetCurrentWorkerThread() { return tls_worker_; }

WorkerThread::WorkerThread()
{
}

WorkerThread::~WorkerThread()
{
    Stop();
}

// ---- 启动 Worker 线程 ----
void WorkerThread::Start(size_t thread_index)
{
    thread_index_ = thread_index;
    thread_name_ = "WorkerIO_" + std::to_string(thread_index);
    running_.store(true);
    thread_ = std::thread(&WorkerThread::WorkerRoutine, this);
}

// ---- 停止 Worker 线程 ----
void WorkerThread::Stop()
{
    if (!running_.exchange(false)) return;

    io_context_.stop();

    if (thread_.joinable())
    {
        thread_.join();
    }
}

// ---- 接收 socket：投递到 io_context_，由调用方的回调创建 session ----
void WorkerThread::AcceptSocket(int64_t fd,
    std::function<void(int64_t, boost::asio::io_context&)> create_session_callback)
{
    boost::asio::post(io_context_,
        [fd, cb = std::move(create_session_callback), &io = io_context_]() mutable
        {
            // 在 Worker 线程上执行回调，由调用方负责创建 session
            cb(fd, io);
        });
}

// ---- Worker 线程入口 ----
void WorkerThread::WorkerRoutine()
{
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(),
        std::wstring(thread_name_.begin(), thread_name_.end()).c_str());
#else
    pthread_setname_np(pthread_self(), thread_name_.c_str());
#endif

    // ---- 第一步：初始化数据库执行器 ----
    sql_exec_ = new SqlExec();
    sql_exec_->Init(GlobalConfig::Instance()->GetDbPath());  // 每个 Worker 各自打开 .db
    tls_sql_exec_ = sql_exec_;
    tls_worker_   = this;

    LOG_INFO("thread", "[WorkerThread] 线程就绪 (SQLite)");

    // 启动事件循环积压探测
    StartBacklogProbe();

    // ---- 第二步：进入事件循环 ----
    try
    {
        io_context_.run();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("thread", "[WorkerThread] io_context::run 异常: {}", e.what());
    }

    // ---- 第三步：清理资源 ----
    tls_sql_exec_ = nullptr;
    delete sql_exec_;
    sql_exec_ = nullptr;

    tls_worker_ = nullptr;

    LOG_INFO("thread", "[WorkerThread] 线程退出");
}

// ---- 启动事件循环积压探测 ----
void WorkerThread::StartBacklogProbe()
{
    backlog_probe_timer_ = std::make_unique<boost::asio::steady_timer>(io_context_);
    ProbeBacklog();
}

// ---- 投递探针，测量 io_context 队列延迟 ----
void WorkerThread::ProbeBacklog()
{
    if (!running_) return;

    auto posted_at = std::chrono::steady_clock::now();

    // 投递一个最简单的任务到 io_context
    boost::asio::post(io_context_, [this, posted_at]()
        {
            auto executed_at = std::chrono::steady_clock::now();
            auto delay_us = std::chrono::duration_cast<std::chrono::microseconds>(
                executed_at - posted_at).count();

            if (delay_us > 2000) {  // > 2ms 说明有积压
                LOG_WARN("perf", "[Backlog] {} queue_delay:{}ms",
                    thread_name_, delay_us / 1000.0);
            }

            // 1 秒后再探
            if (backlog_probe_timer_)
            {
                backlog_probe_timer_->expires_after(std::chrono::seconds(1));
                backlog_probe_timer_->async_wait([this](const boost::system::error_code& ec)
                    {
                        if (!ec) ProbeBacklog();
                    });
            }
        });
}