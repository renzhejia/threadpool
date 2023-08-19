
#include <queue>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <functional>
#include <vector>
#include <thread>
#include <condition_variable>
#include <future>
#include <utility>

template <typename T>
class SafeQueue
{
private:
    std::queue<T> m_queue; // 利用模板函数构造队列

    std::mutex m_mutex; // 访问互斥信号量

public:
    SafeQueue() {}
    SafeQueue(SafeQueue &&other) {}

    ~SafeQueue() {}

    // 判断队列是否为空
    bool empty()
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        return m_queue.empty();
    }

    int size()
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        return m_queue.size();
    }

    // 队列添加元素
    void enqueue(T &t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_queue.emplace(t);
    }

    // 队列取出元素
    bool dequeue(T &t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_queue.empty())
        {
            return false;
        }

        // 取出队首元素，返回队首元素值，并进行右值引用
        t=std::move(m_queue.front());

        //弹出入队的第一个元素
        m_queue.pop();
        
        return true;
    }
};


class ThreadPool
{
    class ThreadWorker // 内置线程工作类
    {
    private:
        int m_id; // 工作id

        ThreadPool *m_pool; // 所属线程池

    public:
        // 构造函数
        ThreadWorker(ThreadPool *pool, const int id) : m_pool(pool), m_id(id)
        {
        }

        // 重载（）操作
        void operator()()
        {
            // 该模板类可用于封装任何可调用对象，包括函数指针、成员函数指针、函数对象、
            // Lambda表达式等，可以让编程者具有更大的编程灵活性和可组合性。
            std::function<void()> func; // 定义基础函数类func

            bool dequeued; // 是否正在取出队列中元素

            if (!m_pool->m_shutdown)
            {
                {
                    // 为当前线程环境加锁，互访问工作线程的休眠和唤醒
                    std::unique_lock<std::mutex> lock(m_pool->m_conditional_mutex);

                    // 如果任务队列为空，阻塞当前线程
                    if (m_pool->m_queue.empty())
                    {
                        // 等待条件变量通知，开启线程
                        m_pool->m_conditional_lock.wait(lock);
                    }

                    // 取出任务队列中的元素
                    dequeued = m_pool->m_queue.dequeue(func);
                }

                // 如果成功取出，执行工作函数
                if (dequeued)
                {
                    func();
                }
            }
        }
    };

    bool m_shutdown; // 线程池是否关闭

    SafeQueue<std::function<void()>> m_queue; // 执行函数安全队列，即任务队列

    std::vector<std::thread> m_threads; // 工作线程队列

    std::mutex m_conditional_mutex; // 线程休眠锁互斥变量

    // 这个类是一个用于阻塞一个或多个线程，直到被他人通知的同步原语，通常与
    // std::unique_lock 一起使用。通过调用 wait 成员函数，可以让线程在等待期间阻塞，
    // 并且在满足特定条件时被唤醒，从而开始执行其他操作。
    std::condition_variable m_conditional_lock; // 线程环境锁，可让线程处于休眠或唤醒状态

public:
    // 线程池构造函数
    ThreadPool(const int n_threads = 4) : m_threads(std::vector<std::thread>(n_threads)), m_shutdown(false)
    {
    }

    // 从语法上来说，它防止了一个类的复制构造函数的默认自动生成
    // 即，这个类不能被拷贝构造
    ThreadPool(const ThreadPool &) = delete;

    // 即这个类不能移动构造
    ThreadPool(ThreadPool &&) = delete;

    ThreadPool &operator=(const ThreadPool &) = delete;

    ThreadPool &operator=(ThreadPool &&) = delete;

    // 加载线程池
    void init()
    {
        for (int i = 0; i < m_threads.size(); i++)
        {
            // 分配工作线程
            m_threads.at(i) = std::thread(ThreadWorker(this, i));
        }
    }

    // 等待所有线程完成它们当前的任务并关闭线程池，即在该函数中等待处理中的任务全部完成后将线程池置于关闭状态。
    void shutdown()
    {
        m_shutdown = true;
        // 通知，唤醒所有工作线程.用于唤醒所有等待与此条件变量关联的锁的线程。
        m_conditional_lock.notify_all();

        for (int i = 0; i < m_threads.size(); i++)
        {
            // 一个线程是否可加入(joinable)是指该线程是否已经被创建并启动，
            // 并且还没有被回收(join)。
            // 若线程可加入，即该线程并未被回收，我们要先将线程一一回收(即join)
            if (m_threads.at(i).joinable())
            {
                m_threads.at(i).join();
            }
        }
    }

    // 将一个函数提交给线程池异步执行,意味着该函数将会在一个新的线程中被执行，
    // 而不会阻塞当前线程的执行。这种方式可以让我们充分利用 CPU 的多核运算能力，提高程序的并发能力和响应速度。
    // future可用来接收异步操作的返回结果，可用get获取异步操作的返回值
    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args) -> std::future<decltype(f(args...))>
    {
        // 创建一个有边界参数的函数，准备执行
        // 这个注释中提到的“有边界参数的函数”指的是一个参数已经被固定的函数。在编写多线程程序时，
        // 我们有时需要将一个函数提交给线程池异步执行，但这个函数需要接收一些参数才能正常工作。
        // 这时候，我们可以使用一个有边界参数的函数来封装这个原本需要参数的函数，并且把参数事先固定下来。
        // 当异步操作开始执行时，这个带有边界参数的函数会把参数传递给原本需要参数的函数，从而完成异步操作。
        //// 连接函数和参数定义，特殊函数类型，避免左右值错误
        std::function<decltype(f(args...))()> func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

        // 将之封装到一个共享指针中，以便能够进行复制构造
        auto task_ptr = std::make_shared<std::packaged_task<decltype(f(args...))()>>(func);

        // 将task_ptr封装为一个无返回值的函数
        std::function<void()> warpper_func = [task_ptr]() {
            (*task_ptr)();
        };

        //队列通用安全封包函数，并压入安全队列
        m_queue.enqueue(warpper_func);

        //唤醒一个等待的线程
        m_conditional_lock.notify_one();

        //返回先前注册的任务指针
        return task_ptr->get_future();
    }
};