#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <memory>
#include <vector>
#include <atomic>
#include <functional>
#include "Global/singletion.h"
#include "WorkerThread.h"

class ThreadPool : public Singleton<ThreadPool>
{
    friend class Singleton<ThreadPool>;

public:
    ~ThreadPool();

    void Start(size_t thread_count);
    void Stop();

    WorkerThread* GetNextWorker();

    template<typename F>
    void PostTask(F&& task)
    {
        auto worker = GetNextWorker();
        if (worker)
            worker->PostTask(std::forward<F>(task));
    }

    static SqlExec* GetCurrentSqlExec() { return ::GetCurrentThreadSqlExec(); }
    static WorkerThread* GetCurrentWorker() { return ::GetCurrentWorkerThread(); }

private:
    ThreadPool() = default;

    std::vector<std::unique_ptr<WorkerThread>> workers_;
    std::atomic<size_t> next_index_{ 0 };
};

#endif // THREADPOOL_H
