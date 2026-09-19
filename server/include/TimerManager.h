#pragma once
#include <queue>
#include <unordered_map>
#include <functional>
#include <chrono>

/** @brief 使用最小堆管理连接空闲超时，并用递增 ID 防止 fd 复用误删。 */
class TimerManager {
public:
    using TimeoutCallback = std::function<void(int)>;

    /** @brief 创建空定时器管理器。 */
    TimerManager();
    /** @brief 释放堆和索引容器。 */
    ~TimerManager() = default;

    // 添加/更新定时器
    void add(int fd, int timeout_ms);
    void update(int fd, int timeout_ms);

    // 删除（连接关闭时调用）
    void remove(int fd);

    // 处理超时
    void handleExpired(TimeoutCallback cb);

    // 返回距离下一个超时的时间（用于 epoll_wait）
    int getNextTimeout();

private:
    using Clock = std::chrono::steady_clock;
    using MS = std::chrono::milliseconds;
    using TimePoint = std::chrono::time_point<Clock>;

    struct TimerNode {
        int fd;
        uint64_t id;       // 防 fd 复用
        TimePoint expire;
        bool deleted;

        bool operator<(const TimerNode& other) const {
            return expire > other.expire; // 小堆
        }
    };

    std::priority_queue<TimerNode> heap_;
    std::unordered_map<int, uint64_t> idMap_; // fd -> 当前有效 id

    uint64_t idGen_; // 全局递增 id

private:
    /** @brief 返回单调时钟的当前时刻。 */
    TimePoint now() const;
};
