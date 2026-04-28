#pragma once

#include "common/IOptionPlugin.hpp"
#include <string>
#include <vector>
#include <map>
#include <log4cplus/logger.h>
#include <termios.h>
#include <unistd.h>

namespace test::logconfig {

/**
 * @brief 日志配置工具插件
 *
 * 分类：UTILITY（不走消费策略）
 *
 * 提供运行时 log4cplus 日志级别查看和修改功能。
 *
 * 命令行用法：
 *   ./qa_cases logconfig --show                       ← 显示所有模块的当前日志级别
 *   ./qa_cases logconfig --set module=vdec,level=DEBUG ← 设置单个模块
 *   ./qa_cases logconfig --set-all WARN               ← 设置所有模块为指定级别
 *   ./qa_cases logconfig --reset                      ← 恢复所有模块到默认级别
 *   ./qa_cases logconfig --tui                        ← TUI 交互模式
 */
class LogConfigPlugin : public IOptionPlugin {
public:
    std::string getName() const override { return "logconfig"; }
    std::string getDescription() const override {
        return "log4cplus 日志级别配置 (查看 / 修改 / TUI)";
    }

    PluginCategory getCategory() const override { return PluginCategory::UTILITY; }

    void registerOptions(CLI::App& app) override;
    void applyTo(WorkerConfig& /*config*/) const override {}

    int run() override;

private:
    int showAllLoggers();
    int setLoggerLevel(const std::string& spec);
    int setAllLoggerLevels(const std::string& level_str);
    int resetLoggers();
    int runTui();

    // TUI 辅助
    struct TermRawMode {
        struct termios old_;
        void enable();
        void disable();
        int getch();
    };

    struct TuiEntry {
        std::string name;
        log4cplus::LogLevel level;
        bool changed = false;
    };

    void tuiCycleLevel(TuiEntry& entry);
    void tuiApply(const std::vector<TuiEntry>& entries);
    static void printOneLine(const TuiEntry& entry, bool is_cursor, std::ostream& out);
    static void printAllList(const std::vector<TuiEntry>& entries, int cursor, std::ostream& out);

    static std::string levelToString(log4cplus::LogLevel level);
    static log4cplus::LogLevel stringToLevel(const std::string& s);

    struct LoggerInfo {
        std::string name;
        log4cplus::LogLevel level;
        bool inherited;
    };
    std::vector<LoggerInfo> collectAllLoggers();

    static const std::vector<std::string> s_known_modules;
    void ensureModulesRegistered();

    bool do_show_ = false;
    bool do_reset_ = false;
    bool do_tui_ = false;
    std::string set_spec_;
    std::string set_all_level_;

    log4cplus::Logger logger_ =
        log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("qa_cases.logconfig"));
};

} // namespace test::logconfig
