#include "TcpConn.h"

#include <algorithm>
#include <errno.h>
#include <unistd.h>

TcpConn::TcpConn() : read_buf_(INITIAL_READ_BUFFER_SIZE) {
    read_idx_ = 0;
    state_ = ConnState::CLOSED;
}

TcpConn::~TcpConn() {
    closeConn();
}

void TcpConn::init(int fd, const sockaddr_in& addr, uint64_t id) {
    fd_ = fd;
    addr_ = addr;
    conn_id_ = id;
    read_idx_ = 0;
    peer_closed_write_ = false;
    state_ = ConnState::IDLE;
}

void TcpConn::closeConn() {
    ConnState old = state_.exchange(ConnState::CLOSING);
    if (old == ConnState::CLOSED || old == ConnState::CLOSING)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
    state_ = ConnState::CLOSED;
}

bool TcpConn::isClosed() const {
    return state_ == ConnState::CLOSED || state_ == ConnState::CLOSING;
}

bool TcpConn::trySetState(ConnState expected, ConnState desired) {
    return state_.compare_exchange_strong(expected, desired);
}

ConnState TcpConn::getState() const {
    return state_.load();
}

bool TcpConn::read() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (true) {
        if (read_idx_ == read_buf_.size()) {
            if (read_buf_.size() >= MAX_READ_BUFFER_SIZE) {
                return false;
            }
            read_buf_.resize(std::min(read_buf_.size() * 2, MAX_READ_BUFFER_SIZE));
        }

        ssize_t bytes = recv(
            fd_,
            read_buf_.data() + read_idx_,
            read_buf_.size() - read_idx_,
            0
        );

        if (bytes > 0) {
            read_idx_ += static_cast<size_t>(bytes);
        } else if (bytes == 0) {
            peer_closed_write_ = true;
            // 对端可能在发送完整请求后半关闭写端。保留已经读到的数据，让
            // HTTP 层完成解析和响应；完全没有数据时才直接结束连接。
            return read_idx_ > 0;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            return false;
        }
    }
    return true;
}

WriteStatus TcpConn::write() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!response_) {
        return WriteStatus::COMPLETE;
    }

    while (true) {
        iovec iov;
        if (!response_->nextIovec(iov)) {
            return WriteStatus::COMPLETE;
        }

        ssize_t len = writev(fd_, &iov, 1);
        if (len > 0) {
            response_->consume(static_cast<size_t>(len));
            if (response_->done()) {
                return WriteStatus::COMPLETE;
            }
            continue;
        }

        if (len == 0) {
            return WriteStatus::AGAIN;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return WriteStatus::AGAIN;
        }
        if (errno == EINTR) {
            continue;
        }
        return WriteStatus::ERROR;
    }
}

bool TcpConn::hasResponse() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return response_ != nullptr;
}

void TcpConn::resetReadState() {
    std::lock_guard<std::mutex> lock(mutex_);
    read_idx_ = 0;
}

void TcpConn::resetWriteState() {
    std::lock_guard<std::mutex> lock(mutex_);
    response_.reset();
}

void TcpConn::reset() {
    resetReadState();
    resetWriteState();
}
