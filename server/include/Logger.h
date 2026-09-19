#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <fstream>

/** @brief 日志严重程度。 */
enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

/** @brief 带后台写线程和按行数轮转的进程日志器。 */
class Logger {
public:
    /** @brief 返回共享日志器实例。 */
    static Logger& instance();
    /** @brief 按配置初始化日志目录、级别、同步方式和轮转行数。 */
    void init(size_t maxLines = 100000,
              const std::string& directory = "./logs",
              LogLevel minimumLevel = LogLevel::INFO,
              bool async = true);
    /** @brief 格式化一条日志并放入写队列。 */
    void log(LogLevel level, const std::string& message);
    /** @brief 刷新队列并停止后台线程。 */
    ~Logger();

private:
    /** @brief 创建未初始化日志器。 */
    Logger();
    /** @brief 后台循环，从队列取日志写入文件。 */
    void asyncWrite();
    /** @brief 把枚举转换为可读级别名。 */
    std::string levelToString(LogLevel level);
    /** @brief 返回当前完整时间字符串。 */
    std::string getTime();
    /** @brief 达到行数阈值后切换日志文件。 */
    void rotateFile();
    /** @brief 返回当前日期字符串。 */
    std::string getDate();

private:
    std::ofstream file_;
    std::queue<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_;
    size_t lineCount_;
    size_t fileIndex_;
    size_t maxLines_;
    LogLevel minimumLevel_;
    bool asyncEnabled_;
    std::string directory_;
    std::string baseFilename_;
    // 必须最后声明：后台线程启动前，其访问的全部状态都应先构造完成。
    std::thread writeThread_;
};
