#include "WebServer.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

static int setNonBlocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);

    return old_option;
}

std::shared_ptr<HttpConn> WebServer::getConn(int fd) {
    std::lock_guard<std::mutex> lock(connMutex_);
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return nullptr;
    }
    return it->second;
}

WebServer::WebServer(int port, int thread_num, int actor_model, int timeout_ms)
    : port_(port),
      actor_model_(actor_model),
      threadpool_(thread_num),
      TIMEOUT_MS(timeout_ms){
    LOG_INFO("WebServer constructing...");
    initSocket();
    LOG_INFO("WebServer init success");
}

void WebServer::initSocket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("socket create failed");
        exit(1);
    }
    LOG_INFO("listen socket created");
    
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR(std::string("bind failed: ") + strerror(errno));
        exit(1);
    }
    
    if (listen(listen_fd_, 1024) < 0) {
        LOG_ERROR("listen failed");
        exit(1);
    }
    setNonBlocking(listen_fd_);

    epoller_.addFd(listen_fd_, EPOLLIN | EPOLLET);

    LOG_INFO("server listen on port " + std::to_string(port_));
}


void WebServer::start(volatile std::sig_atomic_t* stop_flag) {
    while (!stop_flag || !*stop_flag) {
        int timeout = timer_.getNextTimeout();
        int event_cnt = epoller_.wait(timeout);
        LOG_DEBUG("epoll event count = " + std::to_string(event_cnt));
        for (int i = 0; i < event_cnt; i++) {
            epoll_event ev = epoller_.getEvent(i);
            int fd = ev.data.fd;
            uint32_t events = ev.events;

            if (fd == listen_fd_) {
                handleAccept();
                continue;
            }

            else if (events & EPOLLERR) {
                closeConncl(fd);
                LOG_WARN("socket error fd=" + std::to_string(fd));
            }

            else if (events & EPOLLIN) {
                handleRead(fd);
            }

            else if (events & EPOLLOUT) {
                handleWrite(fd);
            }

            // RDHUP 是对端正常关闭写方向，浏览器关闭 keep-alive 连接时很常见。
            // 只有没有待处理读写事件时才关闭，并且不记为 socket 错误。
            else if ((events & EPOLLHUP) || (events & EPOLLRDHUP)) {
                closeConncl(fd);
                LOG_DEBUG("peer closed fd=" + std::to_string(fd));
            }
        }

        timer_.handleExpired([this](int fd){this->closeConncl(fd);});
    }
}

void WebServer::handleAccept() {
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept(listen_fd_, (sockaddr*)&addr, &len);
                
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("accept failed");
            break;
        }

        setNonBlocking(fd);
        auto conn = std::make_shared<HttpConn>();
        uint64_t id = next_conn_id_.fetch_add(1);
        conn->init(fd, addr, id);

        {
            std::lock_guard<std::mutex> lock(connMutex_);
            conns_[fd] = conn;
        }

        epoller_.addFd(fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
        timer_.add(fd, TIMEOUT_MS);

        char ip[64];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        LOG_DEBUG("new client fd=" + std::to_string(fd) + " ip=" + ip + " port=" + std::to_string(ntohs(addr.sin_port)));
    }
}

void WebServer::handleRead(int fd) {

    auto conn = getConn(fd);

    if (!conn) {
        return;
    }

    // =================================
    // Reactor
    // 主线程 read
    // 工作线程 process
    // =================================

    if (actor_model_ == 0) {
        if (!conn->read()) {
            if (conn->peerClosedWrite())
                LOG_DEBUG("peer closed before next request fd=" + std::to_string(fd));
            else
                LOG_WARN("read failed fd=" + std::to_string(fd));
            closeConncl(fd);
            return;
        }
        timer_.update(fd, TIMEOUT_MS);

        ConnState expected = ConnState::IDLE;
        if(!conn->trySetState(expected, ConnState::PROCESSING)) {
            return;
        }

        uint64_t conn_id = conn->getConnId();
        std::weak_ptr<HttpConn> weak_conn = conn;

        threadpool_.enqueue([this, weak_conn, fd, conn_id]() {

            auto conn = weak_conn.lock();
            if (!conn) return;

            if (conn->getConnId() != conn_id) return;

            LOG_DEBUG("process start fd=" + std::to_string(fd));
            conn->process();
            LOG_DEBUG("process finish fd=" + std::to_string(fd));

            if (!conn->hasResponse()) {
                ConnState expected = ConnState::PROCESSING;
                if (conn->trySetState(expected, ConnState::IDLE)) {
                    auto alive = getConn(fd);
                    if (alive &&
                        alive->getConnId() == conn_id &&
                        !alive->isClosed()) {
                        epoller_.modFd(fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
                    }
                }
                return;
            }

            ConnState expected = ConnState::PROCESSING;
            conn->trySetState(expected, ConnState::WRITING);

            auto alive = getConn(fd);
            if (alive &&
                alive->getConnId() == conn_id &&
                !alive->isClosed()) {

                epoller_.modFd(fd, EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
            }
        });
        LOG_DEBUG("Reactor dispatch process fd=" + std::to_string(fd));
    } 

    // =================================
    // 模拟 Proactor
    // 工作线程 read + process
    // =================================

    else {
        ConnState expected = ConnState::IDLE;
        // 防止重复投递
        if(!conn->trySetState(expected, ConnState::READING)) {
            return;
        }
        uint64_t conn_id = conn->getConnId();
        std::weak_ptr<HttpConn> weak_conn = conn;

        threadpool_.enqueue([this, weak_conn, fd, conn_id]() {
            LOG_DEBUG("Proactor read fd=" + std::to_string(fd));
            auto conn = weak_conn.lock();
            if (!conn) return;

            // =========================
            // READING
            // =========================

            if (!conn->read() || conn->getConnId() != conn_id) {
                if (conn->peerClosedWrite())
                    LOG_DEBUG("peer closed before next request fd=" + std::to_string(fd));
                else
                    LOG_WARN("Proactor read failed fd=" + std::to_string(fd));

                ConnState expected = ConnState::READING;
                conn->trySetState(expected, ConnState::CLOSED);

                closeConncl(fd);
                return;
            }

            timer_.update(fd, TIMEOUT_MS);

            // =========================
            // READING -> PROCESSING
            // =========================

            {
                ConnState expected = ConnState::READING;
                if(!conn->trySetState(expected, ConnState::PROCESSING)) {
                    return;
                }
            }

            // =========================
            // PROCESSING
            // =========================

            LOG_DEBUG("process start fd=" + std::to_string(fd));
            if (conn->getConnId() != conn_id) return;
            conn->process();
            LOG_DEBUG("process finish fd=" + std::to_string(fd));

            if (!conn->hasResponse()) {
                ConnState expected = ConnState::PROCESSING;
                if (conn->trySetState(expected, ConnState::IDLE)) {
                    auto alive = getConn(fd);
                    if (alive &&
                        alive->getConnId() == conn_id &&
                        !alive->isClosed()) {
                        epoller_.modFd(fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
                    }
                }
                return;
            }

            // =========================
            // PROCESSING -> WRITING
            // =========================

            {
                ConnState expected = ConnState::PROCESSING;
                if(!conn->trySetState(expected, ConnState::WRITING)) {
                    return;
                }
            }

            // =========================
            // 注册写事件
            // =========================

            auto alive = getConn(fd);

            if (alive && alive->getState() != ConnState::CLOSED && conn->getConnId() == conn_id) {
                epoller_.modFd(fd, EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
            }
        });
    }
}

void WebServer::handleWrite(int fd) {
    auto conn = getConn(fd);
    if (!conn) {
        return;
    }
    uint64_t conn_id = conn->getConnId();
    if (conn->getState() != ConnState::WRITING || conn_id != conn->getConnId())
        return;

    auto status = conn->write();
    

    // ============================
    // 写失败
    // ============================

    if (status == WriteStatus::ERROR) {
        ConnState expected = ConnState::WRITING;
        conn->trySetState(expected, ConnState::CLOSED);
        LOG_ERROR("write error fd=" + std::to_string(fd));
        closeConncl(fd);
        return;
    }
    timer_.update(fd, TIMEOUT_MS);

    // ============================
    // 写完成
    // ============================

    if (status == WriteStatus::COMPLETE) {
        conn->resetWriteState();
        LOG_DEBUG("write complete fd=" + std::to_string(fd));
        if (conn->isKeepAlive() && !conn->peerClosedWrite()) {
            conn->resetReadState();
            ConnState expected = ConnState::WRITING;
            conn->trySetState(expected, ConnState::IDLE);

            auto alive = getConn(fd);
            if (alive && !alive->isClosed() && conn_id == conn->getConnId()) {
                epoller_.modFd(fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
            }
            LOG_DEBUG("keep alive fd=" + std::to_string(fd));
        }

        else {
            closeConncl(fd);
        }
    }

    // ============================
    // 继续写
    // ============================

    else if (status == WriteStatus::AGAIN) {
        auto alive = getConn(fd);
        if (alive && !alive->isClosed() && conn_id == conn->getConnId()) {
            epoller_.modFd(fd, EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
        }
        LOG_DEBUG("write again fd=" + std::to_string(fd));
    }
}

void WebServer::closeConncl(int fd) {
    std::shared_ptr<HttpConn> conn;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        auto it = conns_.find(fd);
        if(it == conns_.end())
            return;

        conn = it->second;
        epoller_.delFd(fd);
        conns_.erase(it);
    } 
    LOG_DEBUG("close connection fd=" + std::to_string(fd));
    conn->closeConn();
}

WebServer::~WebServer() {
    LOG_INFO("WebServer shutting down...");
    threadpool_.shutdown();
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        for (auto& [fd, conn] : conns_) {
            conn->closeConn();
        }
        conns_.clear();
    }

    if (listen_fd_ != -1) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    LOG_INFO("WebServer destroyed");
}
