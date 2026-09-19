#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <future>
#include <condition_variable>
#include <functional>
#include <stdexcept>

/** @brief 固定工作线程数、支持 future 返回值的通用任务池。 */
class ThreadPool {
public:
    /** @brief 创建线程并启动任务循环。 */
    explicit ThreadPool(size_t threadCount);
    /** @brief 停止接收任务并等待工作线程退出。 */
    ~ThreadPool();
    /** @brief 幂等关闭任务池。 */
    void shutdown();

    // 提交任务（支持返回值）
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::invoke_result<F, Args...>::type>;

private:
    std::vector<std::thread> workers;              // 工作线程
    std::queue<std::function<void()>> tasks;       // 任务队列

    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
};

/** @brief 把可调用对象包装为 packaged_task，排队并返回其 future。 */
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) 
    -> std::future<typename std::invoke_result<F, Args...>::type> {

    using return_type = typename std::invoke_result<F, Args...>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> res = task->get_future();

    {
        std::unique_lock<std::mutex> lock(queueMutex);

        if (stop)
            throw std::runtime_error("enqueue on stopped ThreadPool");

        tasks.emplace([task]() {
            (*task)();
        });
    }

    condition.notify_one();
    return res;
}

#endif
