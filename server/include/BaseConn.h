#ifndef BASE_CONN_H
#define BASE_CONN_H

#include <arpa/inet.h>


/** @brief 一次非阻塞写操作的结果。 */
enum class WriteStatus {
    COMPLETE,   // 写完
    AGAIN,      // 未写完（需要EPOLLOUT）
    ERROR       // 出错
};

/** @brief 连接在 Reactor/Proactor 流程中的生命周期状态。 */
enum class ConnState {
    IDLE,          // 空闲，可读
    READING,       // 正在读
    PROCESSING,    // 正在业务处理
    WRITING,       // 正在写
    CLOSING,       // 正在关闭
    CLOSED         // 已关闭
};

/** @brief epoll 管理的连接抽象，统一规定读、写、处理和状态切换接口。 */
class BaseConn {
public:
    /** @brief 通过基类释放具体连接对象。 */
    virtual ~BaseConn() {}
    /** @brief 绑定 socket、对端地址与防 fd 复用的连接编号。 */
    virtual void init(int fd, const sockaddr_in& addr, uint64_t id) = 0;
    /** @brief 幂等关闭连接及其资源。 */
    virtual void closeConn() = 0;
    /** @brief 从非阻塞 socket 读取尽可能多的数据。 */
    virtual bool read() = 0;
    /** @brief 向非阻塞 socket 写当前响应。 */
    virtual WriteStatus write() = 0;
    /** @brief 为下一次请求清理读状态。 */
    virtual void resetReadState() = 0;
    /** @brief 为下一次响应清理写状态。 */
    virtual void resetWriteState() = 0;
    /** @brief 完整重置连接的请求/响应状态。 */
    virtual void reset() = 0;
    /** @brief 在工作线程中执行协议解析和业务处理。 */
    virtual void process() = 0;  // 核心：业务处理
    /** @brief 查询连接是否已经关闭。 */
    virtual bool isClosed() const = 0;
    /** @brief 原子地执行连接状态比较并交换。 */
    virtual bool trySetState(ConnState expected, ConnState desired) = 0;
    /** @brief 返回当前连接状态。 */
    virtual ConnState getState() const = 0;
    /** @brief 返回底层 socket fd。 */
    int getFd() const { return fd_; }

protected:
    int fd_;
    sockaddr_in addr_;
};

#endif
