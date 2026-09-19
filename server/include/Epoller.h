#ifndef EPOLLER_H
#define EPOLLER_H

#include <sys/epoll.h>
#include <vector>

/** @brief 对 Linux epoll 实例和事件缓冲区的 RAII 封装。 */
class Epoller {
public:
    /** @brief 创建 epoll 实例并预分配事件数组。 */
    Epoller(int maxEvent = 1024);
    /** @brief 关闭 epoll fd。 */
    ~Epoller();
    /** @brief 注册 fd 及关注事件。 */
    bool addFd(int fd, uint32_t events);
    /** @brief 修改已注册 fd 的事件掩码。 */
    bool modFd(int fd, uint32_t events);
    /** @brief 从 epoll 中删除 fd。 */
    bool delFd(int fd);
    /** @brief 等待事件并返回就绪数量。 */
    int wait(int timeout = -1);
    /** @brief 取得本轮第 i 个就绪事件。 */
    epoll_event getEvent(int i);

private:
    int epfd_;
    std::vector<epoll_event> events_;
}; 

#endif
