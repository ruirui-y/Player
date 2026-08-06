#include "ThreadPool.h"
#include "WorkerThread.h"
#include "Global/LogManager.h"
#include <boost/asio.hpp>

ThreadPool::~ThreadPool()
{
    Stop();
}

void ThreadPool::Start(size_t thread_count)
{
    if (!workers_.empty()) return;

    if (thread_count == 0)
        thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0) thread_count = 2;

    for (size_t i = 0; i < thread_count; ++i)
    {
        auto worker = std::make_unique<WorkerThread>();
        worker->Start(i);
        workers_.push_back(std::move(worker));
    }

    LOG_INFO("thread", "[ThreadPool] 已启动 {} 个 Worker 线程", thread_count);
}

void ThreadPool::Stop()
{
    for (auto& w : workers_)
        w->Stop();
    workers_.clear();
    LOG_INFO("thread", "[ThreadPool] 已停止");
}

WorkerThread* ThreadPool::GetNextWorker()
{
    size_t count = workers_.size();
    if (count == 0) return nullptr;
    size_t index = next_index_.fetch_add(1, std::memory_order_relaxed) % count;
    return workers_[index].get();
}

// 业务代码直接调用 GetCurrentThreadSqlExec()->Execute() / Query()
