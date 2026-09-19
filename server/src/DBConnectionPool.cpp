#include "DBConnectionPool.h"

#include <iostream>
#include <vector>

DBConnectionPool* DBConnectionPool::getInstance() {
    static DBConnectionPool pool;
    return &pool;
}

DBConnectionPool::DBConnectionPool()
    : maxConn(0), curConn(0), port(0), initialized(false) {}

DBConnectionPool::~DBConnectionPool() { destroyPool(); }

MYSQL* DBConnectionPool::createConnection() const {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) return nullptr;
    if (!mysql_real_connect(conn, host.c_str(), user.c_str(), password.c_str(),
                            dbname.c_str(), port, nullptr, 0)) {
        std::cerr << "mysql_real_connect error: " << mysql_error(conn) << '\n';
        mysql_close(conn);
        return nullptr;
    }
    mysql_set_character_set(conn, "utf8mb4");
    return conn;
}

bool DBConnectionPool::init(const std::string& new_host,
                            const std::string& new_user,
                            const std::string& new_password,
                            const std::string& new_dbname,
                            int new_port,
                            int max_conn) {
    if (new_host.empty() || new_user.empty() || new_dbname.empty() ||
        new_port <= 0 || new_port > 65535 || max_conn <= 0) return false;
    destroyPool();
    host = new_host;
    user = new_user;
    password = new_password;
    dbname = new_dbname;
    port = new_port;
    maxConn = max_conn;

    std::vector<MYSQL*> connected;
    connected.reserve(static_cast<size_t>(max_conn));
    for (int i = 0; i < max_conn; ++i) {
        MYSQL* conn = createConnection();
        if (!conn) {
            for (MYSQL* opened : connected) mysql_close(opened);
            return false;
        }
        connected.push_back(conn);
    }
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (MYSQL* conn : connected) connQueue.push(conn);
        curConn = static_cast<int>(connected.size());
        initialized = true;
    }
    cond.notify_all();
    return true;
}

MYSQL* DBConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(mtx);
    cond.wait(lock, [this] { return !connQueue.empty() || !initialized; });
    if (!initialized || connQueue.empty()) return nullptr;
    MYSQL* conn = connQueue.front();
    connQueue.pop();
    return conn;
}

void DBConnectionPool::releaseConnection(MYSQL* conn) {
    if (!conn) return;
    mysql_rollback(conn);
    mysql_autocommit(conn, true);
    // 不启用已弃用的 MYSQL_OPT_RECONNECT。失效连接在归还时显式关闭并补建，
    // 避免事务执行到一半发生隐式重连而丢失会话状态。
    const bool healthy = mysql_ping(conn) == 0;
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!initialized) {
            mysql_close(conn);
            if (curConn > 0) --curConn;
            return;
        }
        if (healthy) {
            connQueue.push(conn);
            cond.notify_one();
            return;
        }
        mysql_close(conn);
        if (curConn > 0) --curConn;
    }

    // 建连可能较慢，因此在互斥锁之外补建。
    MYSQL* replacement = createConnection();
    if (!replacement) return;
    std::lock_guard<std::mutex> lock(mtx);
    if (initialized) {
        connQueue.push(replacement);
        ++curConn;
        cond.notify_one();
    } else {
        mysql_close(replacement);
    }
}

void DBConnectionPool::destroyPool() {
    std::lock_guard<std::mutex> lock(mtx);
    initialized = false;
    while (!connQueue.empty()) {
        mysql_close(connQueue.front());
        connQueue.pop();
    }
    curConn = 0;
    cond.notify_all();
}

bool DBConnectionPool::isInitialized() const {
    std::lock_guard<std::mutex> lock(mtx);
    return initialized;
}

DBConnectionPoolRAII::DBConnectionPoolRAII(
    MYSQL** conn_pt, DBConnectionPool* connpool)
    : conn_raii(nullptr), pool_raii(connpool) {
    if (pool_raii) conn_raii = pool_raii->getConnection();
    if (conn_pt) *conn_pt = conn_raii;
}

DBConnectionPoolRAII::~DBConnectionPoolRAII() {
    if (conn_raii && pool_raii) pool_raii->releaseConnection(conn_raii);
}
