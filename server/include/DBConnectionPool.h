#ifndef DB_CONNECTION_POOL_H
#define DB_CONNECTION_POOL_H

#include <condition_variable>
#include <mutex>
#include <mysql/mysql.h>
#include <queue>
#include <string>

/** @brief 线程安全的 MySQL 连接池，并在归还连接时清理事务状态。 */
class DBConnectionPool {
public:
    /** @brief 返回进程内唯一的连接池实例。 */
    static DBConnectionPool* getInstance();
    /** @brief 创建 max_conn 条连接；任意一条失败都会关闭已创建连接。 */
    bool init(const std::string& host, const std::string& user,
              const std::string& password, const std::string& dbname,
              int port, int max_conn);
    /** @brief 阻塞借用连接；连接池停止后返回 nullptr。 */
    MYSQL* getConnection();
    /** @brief 清理事务状态并归还连接；失效连接会被关闭并尽量补建。 */
    void releaseConnection(MYSQL* conn);
    /** @brief 停止连接池、唤醒等待者并关闭全部空闲连接。 */
    void destroyPool();
    /** @brief 判断连接池是否已经成功初始化。 */
    bool isInitialized() const;

private:
    /** @brief 构造空连接池。 */
    DBConnectionPool();
    /** @brief 销毁连接池。 */
    ~DBConnectionPool();
    /** @brief 使用已保存的配置创建一条连接。 */
    MYSQL* createConnection() const;

    int maxConn;
    int curConn;
    std::queue<MYSQL*> connQueue;
    mutable std::mutex mtx;
    std::condition_variable cond;
    std::string host;
    std::string user;
    std::string password;
    std::string dbname;
    int port;
    bool initialized;
};

/** @brief 借用 MySQL 连接的 RAII 守卫，异常离开作用域也会自动归还。 */
class DBConnectionPoolRAII {
public:
    /** @brief 从连接池借用连接并写入 conn_pt。 */
    DBConnectionPoolRAII(MYSQL** conn_pt, DBConnectionPool* connpool);
    /** @brief 自动归还成功借到的连接。 */
    ~DBConnectionPoolRAII();

private:
    MYSQL* conn_raii;
    DBConnectionPool* pool_raii;
};

#endif
