#ifndef TCP_CONN_H
#define TCP_CONN_H

#include "BaseConn.h"
#include <cstring>
#include <sys/uio.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>
#include "Response.h"

/** @brief 实现非阻塞 TCP 读写、响应发送和原子连接状态机。 */
class TcpConn : public BaseConn {
public:
    /** @brief 创建尚未绑定 fd 的连接。 */
    TcpConn();
    /** @brief 幂等关闭连接。 */
    virtual ~TcpConn();
    /** @brief 初始化本次 fd 生命周期及连接编号。 */
    void init(int fd, const sockaddr_in& addr, uint64_t id) override;
    /** @brief 关闭 socket 并释放响应。 */
    void closeConn() override;
    /** @brief 循环读取直到 EAGAIN 或错误。 */
    bool read() override;
    /** @brief 发送响应直到完成、EAGAIN 或错误。 */
    WriteStatus write() override;
    /** @brief 查询关闭标志。 */
    bool isClosed() const override;
    /** @brief 原子比较交换连接状态。 */
    bool trySetState(ConnState expected, ConnState desired) override;
    /** @brief 读取连接状态。 */
    ConnState getState() const override;
    /** @brief 清空已读请求数据。 */
    void resetReadState() override;
    /** @brief 清空当前响应。 */
    void resetWriteState() override;
    /** @brief 同时重置读写状态。 */
    void reset() override;
    /** @brief 设置防 fd 复用编号。 */
    void setConnId(uint64_t id) { conn_id_ = id; }
    /** @brief 返回防 fd 复用编号。 */
    uint64_t getConnId() const { return conn_id_; }
    /** @brief 判断业务层是否已经构建响应。 */
    bool hasResponse() const;
    /** @brief 对端是否已正常关闭写方向。已有请求仍可处理并返回响应。 */
    bool peerClosedWrite() const { return peer_closed_write_.load(); }
    /** @brief 由具体协议连接实现业务处理。 */
    virtual void process() override = 0; // 纯虚（交给子类）

protected:
    static const size_t INITIAL_READ_BUFFER_SIZE = 4096;
    static const size_t MAX_READ_BUFFER_SIZE = 16 * 1024 * 1024;
    static const int WRITE_BUFFER_SIZE = 4096;

    std::vector<char> read_buf_;
    size_t read_idx_;

    char write_buf_[WRITE_BUFFER_SIZE];
    std::unique_ptr<HttpResponse> response_;

    uint64_t conn_id_;

    std::atomic<bool> closed_;
    std::atomic<bool> peer_closed_write_{false};
    std::atomic<ConnState> state_;
    mutable std::mutex mutex_;

};

#endif
