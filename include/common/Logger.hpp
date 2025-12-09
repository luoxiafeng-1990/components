/**
 * @file Logger.hpp
 * @brief 统一的日志管理接口（基于 log4cplus）
 * 
 * 功能：
 * - 控制台输出（彩色显示）
 * - 本地文件保存（自动轮转）
 * - 远程TCP日志传输
 * - 精准的日志级别控制
 * 
 * 使用方式：
 *   LOG_INFO("Application started");
 *   LOG_ERROR("Error occurred");
 *   LOG_INFO_FMT("VideoProductionLine created (loop=%s, thread_count=%d)", 
 *                loop ? "enabled" : "disabled", thread_count);
 */

#ifndef COMMON_LOGGER_HPP
#define COMMON_LOGGER_HPP

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <log4cplus/configurator.h>
#include <log4cplus/initializer.h>
#include <string>

// ============================================
// 日志初始化（在 main 函数开始时调用一次）
// ============================================
#define INIT_LOGGER(config_file) \
    do { \
        static log4cplus::Initializer initializer; \
        log4cplus::PropertyConfigurator::doConfigure(LOG4CPLUS_TEXT(config_file)); \
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

