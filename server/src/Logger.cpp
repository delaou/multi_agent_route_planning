#include "Logger.h"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger()
    : running_(true),
      lineCount_(0),
      fileIndex_(0),
      maxLines_(100000),
      minimumLevel_(LogLevel::INFO),
      asyncEnabled_(true),
      directory_("./logs"),
      writeThread_(&Logger::asyncWrite, this) {
}

Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
    if (writeThread_.joinable()) {
        writeThread_.join();
    }
    if (file_.is_open()) {
        file_.close();
    }
}

void Logger::init(size_t maxLines, const std::string& directory,
                  LogLevel minimumLevel, bool async) {
    std::lock_guard<std::mutex> lock(mutex_);
    directory_ = directory.empty() ? "./logs" : directory;
    std::filesystem::create_directories(directory_);
    baseFilename_ = (std::filesystem::path(directory_) /
                     (getDate() + ".log")).string();
    maxLines_ = maxLines;
    minimumLevel_ = minimumLevel;
    asyncEnabled_ = async;
    file_.open(baseFilename_, std::ios::app);
}

void Logger::rotateFile() {
    if (file_.is_open()) {
        file_.close();
    }

    ++fileIndex_;
    std::string newFileName = (std::filesystem::path(directory_) /
        (getDate() + ".log." + std::to_string(fileIndex_))).string();

    file_.open(newFileName, std::ios::app);
    lineCount_ = 0;
}

std::string Logger::levelToString(LogLevel level) {
    switch(level) {
        case LogLevel::DEBUG:
            return "[DEBUG]";
        case LogLevel::INFO:
            return "[INFO ]";
        case LogLevel::WARN:
            return "[WARN ]";
        case LogLevel::ERROR:
            return "[ERROR]";
        default:
            return "[UNKN ]";
    }
}

std::string Logger::getTime() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::stringstream ss;

    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

    return ss.str();
}

void Logger::log(LogLevel level, const std::string& message) {

    if (static_cast<int>(level) < static_cast<int>(minimumLevel_)) return;

    std::stringstream ss;

    ss << getTime()
       << " "
       << levelToString(level)
       << " "
       << message
       << "\n";

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!asyncEnabled_) {
            file_ << ss.str();
            std::cout << ss.str();
            ++lineCount_;
            if (lineCount_ >= maxLines_) rotateFile();
            file_.flush();
            return;
        }
        queue_.push(ss.str());
    }

    cv_.notify_one();
}

void Logger::asyncWrite() {

    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [this] {
            return !queue_.empty() || !running_;
        });
        if (!running_ && queue_.empty()) break;
        while (!queue_.empty()) {
            std::string msg = queue_.front();
            queue_.pop();
            file_ << msg;
            std::cout << msg;
            ++lineCount_;

            // =========================
            // 自动切换日志文件
            // =========================

            if (lineCount_ >= maxLines_) {
                file_.flush();
                rotateFile();
            }
        }
        file_.flush();
    }
}

std::string Logger::getDate() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d");
    return ss.str();
}
