#pragma once

#include "Logger.h"

#define LOG_DEBUG(msg) \
    Logger::instance().log(LogLevel::DEBUG, msg)

#define LOG_INFO(msg) \
    Logger::instance().log(LogLevel::INFO, msg)

#define LOG_WARN(msg) \
    Logger::instance().log(LogLevel::WARN, msg)

#define LOG_ERROR(msg) \
    Logger::instance().log(LogLevel::ERROR, msg)
