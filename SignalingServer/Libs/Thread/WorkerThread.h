#ifndef WORKERTHREAD_H
#define WORKERTHREAD_H

#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <boost/asio.hpp>

class SqlExec;

class WorkerThread
{
public:
    WorkerThread();
    ~WorkerThread();

    void Start(size_t thread_index);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    boost::asio::io_context& GetIoContext() { return io_context_; }

    void AcceptSocket(int64_t fd,
        std::function<void(int64_t, boost::asio::io_context&)> create_session_callback);

    template<typename F>
    void PostTask(F&& task)
    {
        boost::asio::post(io_context_, std::forward<F>(task));
    }

    SqlExec* GetSqlExec() const { return sql_exec_; }

    void StartBacklogProbe();

private:
    void WorkerRoutine();
    void ProbeBacklog();

    std::thread thread_;
    boost::asio::io_context io_context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        work_{ boost::asio::make_work_guard(io_context_) };
    std::atomic<bool> running_{ false };

    size_t thread_index_{ 0 };
    std::string thread_name_;

    SqlExec* sql_exec_{ nullptr };

    std::unique_ptr<boost::asio::steady_timer> backlog_probe_timer_;
};

SqlExec* GetCurrentThreadSqlExec();
WorkerThread* GetCurrentWorkerThread();

#endif // WORKERTHREAD_H
