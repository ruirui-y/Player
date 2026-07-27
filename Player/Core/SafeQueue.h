#ifndef SAFEQUEUE_H
#define SAFEQUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

template<typename T>
class SafeQueue
{
public:
    SafeQueue() = default;
    ~SafeQueue() = default;

    // 禁止拷贝
    SafeQueue(const SafeQueue&) = delete;
    SafeQueue& operator=(const SafeQueue&) = delete;

    // 有上限的阻塞 push（队列满时等待，适用于帧队列）
    void PushMax(T item, size_t max_size)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this, max_size] { return queue_.size() < max_size || stopped_; });
        if (stopped_) return;
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    // 生产者调用
    void Push(T item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    // 消费者调用，阻塞直到有数据或队列停止
    T Pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });

        if (stopped_ && queue_.empty())
            return nullptr;     // 哨兵：结束

        T item = std::move(queue_.front());
        queue_.pop();
        cv_.notify_one();
        return item;
    }

    // 非阻塞弹出
    bool TryPop(T& item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        cv_.notify_one();
        return true;
    }

    size_t Size()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty())
            queue_.pop();
        stopped_ = false;
        serial_ = 0;        // serial 也归零，避免 seek 后 serial 增长失控
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty())
            queue_.pop();
    }

    // 清空队列并自增序列号（seek 时使用）
    void Flush()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty())
            queue_.pop();
        serial_++;
    }

    int serial()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return serial_;
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_{ false };
    int serial_{ 0 };
};

#endif // SAFEQUEUE_H