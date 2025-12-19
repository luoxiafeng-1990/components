/**
 * @file Logger.hpp
 * @brief 统一的日志管理接口（基于 log4cplus）
 * 
 * 功能：
 * - 控制台输出（彩色显示）
 * - 本地文件保存（自动轮转）
 * - 远程TCP日志传输（可选）
 * - 精准的日志级别控制
 * 
 * 使用方式：
 *   INIT_LOGGER();  // 在 main 函数开始时调用一次（无需配置文件）
 *   LOG_INFO("Application started");
 *   LOG_ERROR("Error occurred");
 *   LOG_INFO_FMT("VideoProductionLine created (loop=%s, thread_count=%d)", 
 *                loop ? "enabled" : "disabled", thread_count);
 */

#ifndef COMMON_LOGGER_HPP
#define COMMON_LOGGER_HPP

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <log4cplus/initializer.h>
#include <log4cplus/configurator.h>
#include <log4cplus/consoleappender.h>
#include <log4cplus/layout.h>
#include <string>
#include <memory>

// ============================================
// 日志初始化实现（编程式配置，无需外部文件）
// ============================================
namespace {
    /**
     * @brief 初始化日志系统（使用自定义格式）
     * 
     * 配置内容：
     * - 控制台输出：DEBUG 级别
     * - 自定义格式：[LEVEL] message（级别用[]包围，统一宽度）
     */
    inline void initializeLogger() {
        // 创建 ConsoleAppender
        log4cplus::SharedAppenderPtr appender(new log4cplus::ConsoleAppender());
        
        // 设置自定义 PatternLayout
        // [%-5p] - 日志级别，左对齐，固定5字符宽度，用[]包围
        // %m%n - 消息内容 + 换行
        std::string pattern = "[%-5p] %m%n";
        std::unique_ptr<log4cplus::Layout> layout(new log4cplus::PatternLayout(pattern));
        appender->setLayout(std::move(layout));
        
        // 设置根 Logger
        log4cplus::Logger root = log4cplus::Logger::getRoot();
        root.addAppender(appender);
        root.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
    }
}

// ============================================
// 日志初始化宏（在 main 函数开始时调用一次）
// ============================================
#define INIT_LOGGER() \
    do { \
        static log4cplus::Initializer initializer; \
        static bool initialized = false; \
        if (!initialized) { \
            initializeLogger(); \
            initialized = true; \
        } \
    } while(0)

// ============================================
// 便捷的日志宏（使用根 logger）
// ============================================
#define LOG_TRACE(msg) \
    LOG4CPLUS_TRACE(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(msg))

#define LOG_DEBUG(msg) \
    LOG4CPLUS_DEBUG(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(msg))

#define LOG_INFO(msg) \
    LOG4CPLUS_INFO(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(msg))

#define LOG_WARN(msg) \
    LOG4CPLUS_WARN(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(msg))

#define LOG_ERROR(msg) \
    LOG4CPLUS_ERROR(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(msg))

#define LOG_FATAL(msg) \
    LOG4CPLUS_FATAL(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(msg))

// ============================================
// 带格式化的日志宏（支持 printf 风格格式化）
// ============================================
#define LOG_TRACE_FMT(fmt, ...) \
    LOG4CPLUS_TRACE_FMT(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(fmt), ##__VA_ARGS__)

#define LOG_DEBUG_FMT(fmt, ...) \
    LOG4CPLUS_DEBUG_FMT(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(fmt), ##__VA_ARGS__)

#define LOG_INFO_FMT(fmt, ...) \
    LOG4CPLUS_INFO_FMT(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(fmt), ##__VA_ARGS__)

#define LOG_WARN_FMT(fmt, ...) \
    LOG4CPLUS_WARN_FMT(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(fmt), ##__VA_ARGS__)

#define LOG_ERROR_FMT(fmt, ...) \
    LOG4CPLUS_ERROR_FMT(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(fmt), ##__VA_ARGS__)

#define LOG_FATAL_FMT(fmt, ...) \
    LOG4CPLUS_FATAL_FMT(log4cplus::Logger::getRoot(), LOG4CPLUS_TEXT(fmt), ##__VA_ARGS__)

#endif // COMMON_LOGGER_HPP


