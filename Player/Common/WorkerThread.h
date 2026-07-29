#ifndef WORKERTHREAD_H
#define WORKERTHREAD_H

#include <QThread>
#include <QSharedPointer>
#include <QSemaphore>
#include <QMetaObject>
#include <atomic>

class WorkerThread : public QThread
{
    Q_OBJECT
public:
    explicit WorkerThread(QObject* parent = nullptr) : QThread(parent) {}
    ~WorkerThread() override {}

    void WaitReady() { ready_.acquire(); }

    QObject* Dispatcher() const { return dispatcher_.load(std::memory_order_acquire); }

public:
    // 终极泛型创建函数
    // 参数1：你要创建的对象类型 T
    // 参数2：你要使用的智能指针类型 SmartPtr (默认是 QSharedPointer)
    // 参数3：构造函数的参数 Args...
    template<class T, template<typename> class SmartPtr = QSharedPointer, class... Args>
    SmartPtr<T> CreateQObject(Args&&... args)
    {
        static_assert(std::is_base_of_v<QObject, T>, "T must derive from QObject");

        QObject* d = Dispatcher();
        if (!d) return {};

        // 防弹版删除器 (完美兼容 QSharedPointer 和 std::shared_ptr)
        auto safeDeleter = [](T* obj)
            {
                if (!obj) return;
                QThread* objThread = obj->thread();

                if (!objThread || !objThread->isRunning() || objThread->isFinished()) {
                    delete obj;
                }
                else {
                    obj->deleteLater();
                }
            };

        // 线程内调用：直接 new
        if (QThread::currentThread() == d->thread())
        {
            T* raw = new T(std::forward<Args>(args)...);
            return SmartPtr<T>(raw, safeDeleter);                               // 泛型构造
        }

        SmartPtr<T> out;
        QMetaObject::invokeMethod(d, [&]()
            {
                T* raw = new T(std::forward<Args>(args)...);
                out = SmartPtr<T>(raw, safeDeleter);                            // 泛型构造
            }, Qt::BlockingQueuedConnection);

        return out;
    }

protected:
    void run() override;

signals:
    void SigReady();

private:
    std::atomic<QObject*> dispatcher_{ nullptr };
    QSemaphore ready_{ 0 };
};

#endif // WORKERTHREAD_H
