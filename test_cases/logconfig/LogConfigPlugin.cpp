#include "logconfig/LogConfigPlugin.hpp"
#include "../common/third_party/CLI11.hpp"
#include <log4cplus/loggingmacros.h>
#include <log4cplus/hierarchy.h>
#include <log4cplus/spi/loggerimpl.h>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <sstream>

namespace test::logconfig {

// ============================================================
// CLI 注册
// ============================================================

void LogConfigPlugin::registerOptions(CLI::App& app) {
    app.add_flag("--show,-s", do_show_,
        "显示所有模块的当前日志级别");

    app.add_option("--set", set_spec_,
        "设置指定模块日志级别 (格式: module=xxx,level=DEBUG)");

    app.add_option("--set-all", set_all_level_,
        "设置所有模块到指定级别 (TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF)");

    app.add_flag("--reset", do_reset_,
        "恢复所有模块到 INFO 默认级别");

    app.add_flag("--tui,-t", do_tui_,
        "启动 TUI 交互模式（方向键 + Enter 选择）");
}

// ============================================================
// run()
// ============================================================

int LogConfigPlugin::run() {
    if (do_show_) return showAllLoggers();
    if (!set_spec_.empty()) return setLoggerLevel(set_spec_);
    if (!set_all_level_.empty()) return setAllLoggerLevels(set_all_level_);
    if (do_reset_) return resetLoggers();
    if (do_tui_) return runTui();

    return showAllLoggers();
}

// ============================================================
// 收集所有 logger
// ============================================================

std::vector<LogConfigPlugin::LoggerInfo> LogConfigPlugin::collectAllLoggers() {
    std::vector<LoggerInfo> result;

    auto& hierarchy = log4cplus::Logger::getDefaultHierarchy();
    auto loggers = hierarchy.getCurrentLoggers();

    for (auto& logger : loggers) {
        LoggerInfo info;
        info.name = LOG4CPLUS_TSTRING_TO_STRING(logger.getName());
        info.level = logger.getChainedLogLevel();
        info.inherited = (logger.getLogLevel() == log4cplus::NOT_SET_LOG_LEVEL);
        result.push_back(info);
    }

    std::sort(result.begin(), result.end(),
        [](const LoggerInfo& a, const LoggerInfo& b) {
            return a.name < b.name;
        });

    return result;
}

// ============================================================
// 显示所有 logger
// ============================================================

int LogConfigPlugin::showAllLoggers() {
    auto loggers = collectAllLoggers();

    std::cout << "\n"
              << "┌──────────────────────────────────────────────┬──────────┬───────────┐\n"
              << "│ Logger Name                                  │ Level    │ Inherited │\n"
              << "├──────────────────────────────────────────────┼──────────┼───────────┤\n";

    for (auto& info : loggers) {
        std::string display_name = info.name;
        if (display_name.length() > 44)
            display_name = "..." + display_name.substr(display_name.length() - 41);

        std::cout << "│ " << std::left << std::setw(44) << display_name
                  << " │ " << std::setw(8) << levelToString(info.level)
                  << " │ " << std::setw(9)
                  << (info.inherited ? "yes" : "no")
                  << " │\n";
    }

    std::cout << "└──────────────────────────────────────────────┴──────────┴───────────┘\n"
              << "\n总计: " << loggers.size() << " 个模块\n\n";

    return 0;
}

// ============================================================
// 设置单个 logger 级别
// ============================================================

int LogConfigPlugin::setLoggerLevel(const std::string& spec) {
    std::string module_name, level_str;

    std::istringstream ss(spec);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        auto key = token.substr(0, eq);
        auto val = token.substr(eq + 1);
        if (key == "module") module_name = val;
        else if (key == "level") level_str = val;
    }

    if (module_name.empty() || level_str.empty()) {
        std::cerr << "格式错误。用法: --set module=xxx,level=DEBUG\n";
        return 1;
    }

    auto level = stringToLevel(level_str);
    if (level == log4cplus::NOT_SET_LOG_LEVEL) {
        std::cerr << "未知级别: " << level_str << "\n";
        return 1;
    }

    auto& hierarchy = log4cplus::Logger::getDefaultHierarchy();
    auto loggers = hierarchy.getCurrentLoggers();

    bool found = false;
    for (auto& logger : loggers) {
        std::string name = LOG4CPLUS_TSTRING_TO_STRING(logger.getName());
        if (name.find(module_name) != std::string::npos) {
            logger.setLogLevel(level);
            std::cout << "已设置 " << name << " → "
                      << levelToString(level) << "\n";
            found = true;
        }
    }

    if (!found) {
        std::cerr << "未找到匹配 '" << module_name << "' 的模块\n";
        return 1;
    }
    return 0;
}

// ============================================================
// 设置所有 logger
// ============================================================

int LogConfigPlugin::setAllLoggerLevels(const std::string& level_str) {
    auto level = stringToLevel(level_str);
    if (level == log4cplus::NOT_SET_LOG_LEVEL) {
        std::cerr << "未知级别: " << level_str << "\n";
        return 1;
    }

    auto& hierarchy = log4cplus::Logger::getDefaultHierarchy();
    auto loggers = hierarchy.getCurrentLoggers();

    int count = 0;
    for (auto& logger : loggers) {
        logger.setLogLevel(level);
        count++;
    }

    log4cplus::Logger::getRoot().setLogLevel(level);

    std::cout << "已将 " << count << " 个模块日志级别设为 "
              << levelToString(level) << "\n";
    return 0;
}

// ============================================================
// 重置
// ============================================================

int LogConfigPlugin::resetLoggers() {
    auto& hierarchy = log4cplus::Logger::getDefaultHierarchy();
    auto loggers = hierarchy.getCurrentLoggers();

    for (auto& logger : loggers) {
        logger.setLogLevel(log4cplus::NOT_SET_LOG_LEVEL);
    }

    log4cplus::Logger::getRoot().setLogLevel(log4cplus::INFO_LOG_LEVEL);
    std::cout << "已恢复所有模块到默认级别 (INFO)\n";
    return 0;
}

// ============================================================
// TUI 交互模式（简化版：逐行选择）
// ============================================================

int LogConfigPlugin::runTui() {
    std::cout << "\n=== log4cplus 日志配置 (TUI 模式) ===\n\n";

    auto loggers = collectAllLoggers();

    const char* levels[] = {
        "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"
    };
    const int n_levels = 7;

    for (size_t i = 0; i < loggers.size(); i++) {
        std::cout << "[" << (i + 1) << "/" << loggers.size() << "] "
                  << loggers[i].name
                  << " (当前: " << levelToString(loggers[i].level) << ")\n"
                  << "  可选: ";
        for (int j = 0; j < n_levels; j++) {
            std::cout << (j + 1) << "." << levels[j] << "  ";
        }
        std::cout << "0.跳过\n"
                  << "  输入选择 [0]: ";

        std::string input;
        std::getline(std::cin, input);

        if (input.empty() || input == "0") continue;

        int choice = 0;
        try { choice = std::stoi(input); } catch (...) { continue; }

        if (choice < 1 || choice > n_levels) continue;

        auto level = stringToLevel(levels[choice - 1]);
        auto logger = log4cplus::Logger::getInstance(
            LOG4CPLUS_STRING_TO_TSTRING(loggers[i].name));
        logger.setLogLevel(level);
        std::cout << "  → " << loggers[i].name << " 设为 "
                  << levels[choice - 1] << "\n\n";
    }

    std::cout << "\n=== 配置完成 ===\n";
    showAllLoggers();
    return 0;
}

// ============================================================
// 辅助函数
// ============================================================

std::string LogConfigPlugin::levelToString(log4cplus::LogLevel level) {
    if (level == log4cplus::TRACE_LOG_LEVEL) return "TRACE";
    if (level == log4cplus::DEBUG_LOG_LEVEL) return "DEBUG";
    if (level == log4cplus::INFO_LOG_LEVEL)  return "INFO";
    if (level == log4cplus::WARN_LOG_LEVEL)  return "WARN";
    if (level == log4cplus::ERROR_LOG_LEVEL) return "ERROR";
    if (level == log4cplus::FATAL_LOG_LEVEL) return "FATAL";
    if (level == log4cplus::OFF_LOG_LEVEL)   return "OFF";
    return "NOT_SET";
}

log4cplus::LogLevel LogConfigPlugin::stringToLevel(const std::string& s) {
    std::string upper = s;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "TRACE") return log4cplus::TRACE_LOG_LEVEL;
    if (upper == "DEBUG") return log4cplus::DEBUG_LOG_LEVEL;
    if (upper == "INFO")  return log4cplus::INFO_LOG_LEVEL;
    if (upper == "WARN")  return log4cplus::WARN_LOG_LEVEL;
    if (upper == "ERROR") return log4cplus::ERROR_LOG_LEVEL;
    if (upper == "FATAL") return log4cplus::FATAL_LOG_LEVEL;
    if (upper == "OFF")   return log4cplus::OFF_LOG_LEVEL;
    return log4cplus::NOT_SET_LOG_LEVEL;
}

} // namespace test::logconfig
