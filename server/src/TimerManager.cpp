#include "TimerManager.h"

TimerManager::TimerManager() : idGen_(0) {}

TimerManager::TimePoint TimerManager::now() const {
    return Clock::now();
}

// 添加定时器
void TimerManager::add(int fd, int timeout_ms) {
    uint64_t id = ++idGen_;
    idMap_[fd] = id;

    TimerNode node;
    node.fd = fd;
    node.id = id;
    node.expire = now() + MS(timeout_ms);
    node.deleted = false;

    heap_.push(node);
}

// 更新 = 重新添加（lazy delete）
void TimerManager::update(int fd, int timeout_ms) {
    add(fd, timeout_ms);
}

// 删除（逻辑删除）
void TimerManager::remove(int fd) {
    idMap_.erase(fd);
}

// 处理超时连接
void TimerManager::handleExpired(TimeoutCallback cb) {
    auto now_time = now();

    while (!heap_.empty()) {
        TimerNode node = heap_.top();

        // 判断是否已经被删除或被覆盖
        auto it = idMap_.find(node.fd);
        if (it == idMap_.end() || it->second != node.id) {
            heap_.pop();
            continue;
        }

        // 未超时
        if (node.expire > now_time) {
            break;
        }

        // 超时 → 执行回调
        cb(node.fd);

        // 删除
        idMap_.erase(node.fd);
        heap_.pop();
    }
}

// 获取 epoll wait timeout
int TimerManager::getNextTimeout() {
    while (!heap_.empty()) {
        TimerNode node = heap_.top();

        auto it = idMap_.find(node.fd);
        if (it == idMap_.end() || it->second != node.id) {
            heap_.pop();
            continue;
        }

        auto diff = std::chrono::duration_cast<MS>(node.expire - now()).count();
        return diff > 0 ? diff : 0;
    }

    return -1; // 没有定时器 → 阻塞等待
}