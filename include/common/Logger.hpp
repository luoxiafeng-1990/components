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
#include <log4cplus/fileappender.h>
#include <string>
#include <memory>
#include <vector>
#include <fstream>
#include <sys/stat.h>

// ============================================
// 日志初始化实现（支持配置文件或编程式配置）
// ============================================
namespace {
    /**
     * @brief 检查文件是否存在
     */
    inline bool fileExists(const std::string& path) {
        struct stat buffer;
        return (stat(path.c_str(), &buffer) == 0);
    }

    /**
     * @brief 初始化日志系统
     * 
     * 加载优先级：
     * 1. ./logger.properties（当前目录）
     * 2. /etc/logger.properties（系统配置）
     * 3. 编程式默认配置（如果配置文件都不存在）
     * 
     * 配置文件格式示例：
     *   log4cplus.rootLogger=INFO, CONSOLE
     *   log4cplus.appender.CONSOLE=log4cplus::ConsoleAppender
     *   log4cplus.appender.CONSOLE.layout=log4cplus::PatternLayout
     *   log4cplus.appender.CONSOLE.layout.ConversionPattern=[%D{%Y-%m-%d %H:%M:%S.%q}] [%c] [%-5p] %m%n
     *   log4cplus.logger.components.MultiWorker=DEBUG
     *   log4cplus.logger.components.Worker.Rtsp=TRACE
     */
    inline void initializeLogger() {
        // 尝试加载配置文件
        std::vector<std::string> config_paths = {
            "./logger.properties",           // 当前目录（优先）
            "/etc/logger.properties",        // 系统配置
            "../logger.properties",          // 上级目录
            "logger.properties"              // 备选
        };
        
        bool config_loaded = false;
        for (const auto& config_path : config_paths) {
            if (fileExists(config_path)) {
                try {
                    log4cplus::PropertyConfigurator::doConfigure(LOG4CPLUS_TEXT(config_path.c_str()));
                    // 配置文件加载成功（静默，不输出提示信息）
                    config_loaded = true;
                    break;
                } catch (const std::exception& e) {
                    // 配置文件格式错误，继续尝试下一个
                    continue;
                }
            }
        }
        
        // 如果没有找到配置文件，使用编程式默认配置
        if (!config_loaded) {
            // 创建 ConsoleAppender
            log4cplus::SharedAppenderPtr appender(new log4cplus::ConsoleAppender());
            
            // 设置自定义 PatternLayout
            // [%D{...}] - 时间戳（年-月-日 时:分:秒.毫秒）
            // [%c] - Logger名称（显示完整层次路径，如 components.Worker.Rtsp），无固定宽度
            // [%-5p] - 日志级别，左对齐，固定5字符宽度（保持对齐）
            // %m%n - 消息内容 + 换行
            std::string pattern = "[%D{%Y-%m-%d %H:%M:%S.%q}] [%c] [%-5p] %m%n";
            std::unique_ptr<log4cplus::Layout> layout(new log4cplus::PatternLayout(pattern));
            appender->setLayout(std::move(layout));
            
            // 设置根 Logger
            log4cplus::Logger root = log4cplus::Logger::getRoot();
            root.addAppender(appender);
            root.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
            
            // 静默使用默认配置，不输出提示信息
        }
    }
}

// ============================================
// RAII 日志守卫（推荐：在 main 函数栈上构造）
// ============================================

/**
 * @brief RAII 日志生命周期管理
 *
 * 构造时初始化 log4cplus 并加载配置，析构时自动 shutdown。
 * 用法：在 main() 开头声明一个栈变量即可。
 *
 * @code
 *   int main(int argc, char* argv[]) {
 *       LoggerGuard logger_guard;
 *       // ... 程序逻辑 ...
 *   }  // 离开作用域时自动 shutdown，确保日志刷盘
 * @endcode
 */
class LoggerGuard {
public:
    LoggerGuard() { initializeLogger(); }
    ~LoggerGuard() = default;

    LoggerGuard(const LoggerGuard&) = delete;
    LoggerGuard& operator=(const LoggerGuard&) = delete;

private:
    log4cplus::Initializer initializer_;
};

// 向后兼容：保留宏供旧代码使用（新代码请使用 LoggerGuard）
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


