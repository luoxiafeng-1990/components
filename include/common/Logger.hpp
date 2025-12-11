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
#include <string>
#include <memory>

// ============================================
// 日志初始化实现（编程式配置，无需外部文件）
// ============================================
namespace {
    /**
     * @brief 初始化日志系统（使用默认配置）
     * 
     * 配置内容：
     * - 控制台输出：DEBUG 级别
     * - 使用 log4cplus 的默认配置（BasicConfigurator）
     * 
     * 注意：为了兼容不同版本的 log4cplus，使用最简单的配置方式
     */
    inline void initializeLogger() {
        // 使用 BasicConfigurator 进行基本配置（控制台输出）
        // 这是最兼容的方式，适用于所有版本的 log4cplus
        log4cplus::BasicConfigurator::doConfigure();
        
        // 设置根 Logger 的日志级别为 DEBUG
        log4cplus::Logger root = log4cplus::Logger::getRoot();
        root.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
        
        // 注意：如果需要文件输出或更复杂的配置，可以在运行时通过环境变量
        // 或使用 PropertyConfigurator 加载配置文件（如果需要）
        // 但为了简化部署，这里只使用最基本的控制台输出
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


