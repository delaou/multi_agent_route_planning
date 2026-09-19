#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "Epoller.h"
#include "HttpConn.h"
#include "ThreadPool.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include "TimerManager.h"
#include "Logger.h"
#include "LogMacro.h"
#include <csignal>

/** @brief 组合 socket、epoll、线程池和定时器的 HTTP 服务器主循环。 */
class WebServer {
public:
    /** @brief 保存配置并创建运行所需组件。 */
    WebServer(int port, int thread_num, int actor_model, int timeout_ms);
    /** @brief 关闭监听 socket 和现存连接。 */
    ~WebServer();
    /** @brief 运行事件循环，stop_flag 非零时安全退出。 */
    void start(volatile std::sig_atomic_t* stop_flag = nullptr);

private:
    /** @brief 创建、配置并监听 TCP socket。 */
    void initSocket();
    /** @brief 接受当前所有等待中的客户端。 */
    void handleAccept();
    /** @brief 根据并发模型安排指定连接的读取。 */
    void handleRead(int fd);
    /** @brief 发送响应并重新注册后续事件。 */
    void handleWrite(int fd);
    /** @brief 从连接表移除并关闭指定 fd。 */
    void closeConncl(int fd);
    /** @brief 线程安全取得 fd 对应的共享连接对象。 */
    std::shared_ptr<HttpConn> getConn(int fd);

private:
    int port_;
    int listen_fd_;
    int actor_model_;

    Epoller epoller_;
    ThreadPool threadpool_;
    TimerManager timer_;
    const int TIMEOUT_MS;

    std::atomic<uint64_t> next_conn_id_{1};

    std::unordered_map<int, std::shared_ptr<HttpConn>> conns_;
    std::mutex connMutex_;
};

#endif
