#include "logconfig/LogConfigPlugin.hpp"
#include "../common/third_party/CLI11.hpp"
#include <log4cplus/loggingmacros.h>
#include <log4cplus/hierarchy.h>
#include <log4cplus/spi/loggerimpl.h>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <sys/ioctl.h>

namespace test::logconfig {

// ============================================================
// 已知模块列表（代码中所有显式 Logger::getInstance 使用的名称）
// ============================================================

const std::vector<std::string> LogConfigPlugin::s_known_modules = {
    // test_cases 插件 logger
    "qa_cases.logconfig",
    "qa_cases.memleak",
    "qa_cases.vdec",
    "qa_cases.venc",
    "qa_cases.pp",
    "qa_cases.save",
    "qa_cases.display",
    "qa_cases.npu",
    "qa_cases.opencv",
    "qa_cases.preview",
    "qa_cases.record",
    "qa_cases.writer",
    // source 模块 logger
    "components.Worker",
    "components.Worker.Decode",
    "components.Worker.Encode",
    "components.Worker.Recorder",
    "components.MultiWorker",
    "components.WorkerSyncCoordinator",
    "components.WorkerFactory",
    "components.VideoProductionLine",
    "components.Topology",
    "components.BufferPool",
    "components.PoolBuilder.AVFrame",
    "components.PoolBuilder.Mat",
    "components.PoolBuilder.ContinuousPhysical",
    "consumer.BufferConsumerService",
    "consumer.Consumer.JpegEncode",
    "components.BufferWriter",
    "components.Display.TacoVO",
    "components.Display.SharedContext",
    "components.Display.TacoPro",
    "components.Display.OSD",
    "components.DataSource.File",
    "components.DataSource.Rtsp",
    "components.RawFrameSource.File",
    "components.RawFrameSource.Buffer",
    "components.EncodedPacketSourceFromBuffer",
    "components.MemoryProvider.Taco",
    "components.Monitor.Performance",
    "components.Util.Timer",
};

void LogConfigPlugin::ensureModulesRegistered() {
    for (const auto& name : s_known_modules) {
        log4cplus::Logger::getInstance(LOG4CPLUS_STRING_TO_TSTRING(name));
    }
}

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
    ensureModulesRegistered();

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
              << "+------------------------------------------------+----------+-----------+\n"
              << "| Logger Name                                    | Level    | Inherited |\n"
              << "+------------------------------------------------+----------+-----------+\n";

    for (auto& info : loggers) {
        std::string display_name = info.name;
        if (display_name.length() > 46)
            display_name = "..." + display_name.substr(display_name.length() - 43);

        std::cout << "| " << std::left << std::setw(46) << display_name
                  << " | " << std::setw(8) << levelToString(info.level)
                  << " | " << std::setw(9)
                  << (info.inherited ? "yes" : "no")
                  << " |\n";
    }

    std::cout << "+------------------------------------------------+----------+-----------+\n"
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
// TUI 交互模式（纯文本风格，兼容串口终端）
// ============================================================

// ============================================================
// TermRawMode
// ============================================================

void LogConfigPlugin::TermRawMode::enable() {
    tcgetattr(STDIN_FILENO, &old_);
    struct termios raw = old_;
    // 只关闭输入回显和行缓冲，保留 OPOST 让 \n 正确转为 \r\n
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void LogConfigPlugin::TermRawMode::disable() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_);
}

int LogConfigPlugin::TermRawMode::getch() {
    unsigned char ch = 0;
    if (read(STDIN_FILENO, &ch, 1) == 1)
        return ch;
    return -1;
}

// ============================================================
// 终端行数检测
// ============================================================

int LogConfigPlugin::getTerminalRows() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;  // 串口终端默认
}

// ============================================================
// 全屏渲染（menuconfig 风格）
// ============================================================

void LogConfigPlugin::renderFullScreen(const std::vector<TuiEntry>& entries,
                                        int cursor, int scroll_offset, int visible) {
    std::ostringstream buf;

    // 光标归位 + 清屏
    buf << "\033[H\033[2J";

    // 标题
    buf << "\n";
    buf << "  \033[1m======== Log4cplus 日志级别配置 ========\033[0m\n";
    buf << "\n";

    int total = (int)entries.size();
    int end   = std::min(scroll_offset + visible, total);

    // 找最长模块名用于对齐
    size_t max_name = 0;
    for (int i = scroll_offset; i < end; i++)
        max_name = std::max(max_name, entries[i].name.size());
    if (max_name > 52) max_name = 52;

    for (int i = scroll_offset; i < end; i++) {
        bool sel = (i == cursor);
        const auto& e = entries[i];

        if (sel) buf << "\033[7m";  // 反显

        buf << (sel ? " > " : "   ");

        // [ LEVEL  ]
        std::string lvl = levelToString(e.level);
        while (lvl.size() < 6) lvl += ' ';
        buf << "[ " << lvl << " ]  ";

        // 模块名，右补空格对齐
        std::string name = e.name;
        if (name.size() > max_name)
            name = name.substr(0, max_name);
        buf << name;
        for (size_t p = name.size(); p < max_name; p++) buf << ' ';

        buf << (e.changed ? "  *" : "   ");

        if (sel) buf << "\033[0m";
        buf << "\033[K\n";  // 清行尾 + 换行
    }

    // 滚动指示
    if (total > visible) {
        buf << "\n  (" << (cursor + 1) << "/" << total << ")";
        if (scroll_offset > 0)  buf << "  ↑ more";
        if (end < total)        buf << "  ↓ more";
        buf << "\n";
    } else {
        buf << "\n";
    }

    // 操作提示
    buf << "  ----------------------------------------\n";
    buf << "  \033[1mUp/Down\033[0m 移动  "
        << "\033[1mSpace\033[0m 切换级别  "
        << "\033[1mr\033[0m 重置  "
        << "\033[1ms\033[0m 全设  "
        << "\033[1mq\033[0m 退出并应用\n";

    // 已修改计数
    int changed = 0;
    for (const auto& e : entries)
        if (e.changed) changed++;
    if (changed > 0)
        buf << "  \033[1;33m已修改: " << changed << " 个模块\033[0m\n";

    std::cout << buf.str() << std::flush;
}

// ============================================================
// 循环切换级别
// ============================================================

void LogConfigPlugin::tuiCycleLevel(TuiEntry& entry) {
    static const log4cplus::LogLevel cycle[] = {
        log4cplus::OFF_LOG_LEVEL,
        log4cplus::FATAL_LOG_LEVEL,
        log4cplus::ERROR_LOG_LEVEL,
        log4cplus::WARN_LOG_LEVEL,
        log4cplus::INFO_LOG_LEVEL,
        log4cplus::DEBUG_LOG_LEVEL,
        log4cplus::TRACE_LOG_LEVEL,
    };
    static const int n = 7;

    for (int i = 0; i < n; i++) {
        if (entry.level == cycle[i]) {
            entry.level = cycle[(i + 1) % n];
            entry.changed = (entry.level != log4cplus::INFO_LOG_LEVEL);
            return;
        }
    }
    entry.level = log4cplus::INFO_LOG_LEVEL;
    entry.changed = false;
}

// ============================================================
// 应用
// ============================================================

void LogConfigPlugin::tuiApply(const std::vector<TuiEntry>& entries) {
    for (const auto& entry : entries) {
        if (!entry.changed) continue;
        auto logger = log4cplus::Logger::getInstance(
            LOG4CPLUS_STRING_TO_TSTRING(entry.name));
        logger.setLogLevel(entry.level);
    }
}

// ============================================================
// 主 TUI 入口（全屏 menuconfig 风格）
// ============================================================

int LogConfigPlugin::runTui() {
    ensureModulesRegistered();

    auto raw_loggers = collectAllLoggers();
    std::vector<TuiEntry> entries;
    for (auto& r : raw_loggers) {
        TuiEntry e;
        e.name    = r.name;
        e.level   = r.level;
        e.changed = false;
        entries.push_back(e);
    }

    if (entries.empty()) {
        std::cerr << "没有发现任何 logger 模块\n";
        return 1;
    }

    // 进入 raw 模式
    TermRawMode term;
    term.enable();

    int cursor        = 0;
    int scroll_offset = 0;
    int term_rows     = getTerminalRows();
    int visible       = std::max(4, term_rows - 10); // 留给标题和底栏
    bool running      = true;

    // 首次全屏渲染
    renderFullScreen(entries, cursor, scroll_offset, visible);

    while (running) {
        int ch;
        while ((ch = term.getch()) == -1) { /* poll */ }

        if (ch == 'q' || ch == 'Q') {
            running = false;

        } else if (ch == 27) {
            // ESC 序列：方向键 = ESC [ A/B
            int ch2 = term.getch();
            if (ch2 == '[') {
                int ch3 = term.getch();
                if (ch3 == 'A') {           // Up
                    if (cursor > 0) cursor--;
                    if (cursor < scroll_offset)
                        scroll_offset = cursor;
                } else if (ch3 == 'B') {    // Down
                    if (cursor < (int)entries.size() - 1) cursor++;
                    if (cursor >= scroll_offset + visible)
                        scroll_offset = cursor - visible + 1;
                }
            } else if (ch2 == -1) {
                // 纯 ESC 键，退出
                running = false;
                continue;
            }
            renderFullScreen(entries, cursor, scroll_offset, visible);

        } else if (ch == ' ' || ch == '\t') {
            tuiCycleLevel(entries[cursor]);
            renderFullScreen(entries, cursor, scroll_offset, visible);

        } else if (ch == 'r' || ch == 'R') {
            entries[cursor].level   = log4cplus::INFO_LOG_LEVEL;
            entries[cursor].changed = false;
            renderFullScreen(entries, cursor, scroll_offset, visible);

        } else if (ch == 's' || ch == 'S') {
            auto cur_level = entries[cursor].level;
            for (auto& e : entries) {
                if (!e.changed) {
                    e.level   = cur_level;
                    e.changed = true;
                }
            }
            renderFullScreen(entries, cursor, scroll_offset, visible);
        }
    }

    // 恢复终端
    term.disable();

    // 清屏后显示结果
    std::cout << "\033[H\033[2J";

    int changed_count = 0;
    for (const auto& e : entries)
        if (e.changed) changed_count++;

    if (changed_count > 0) {
        tuiApply(entries);
        std::cout << "已应用 " << changed_count << " 个模块的日志级别变更\n\n";
    } else {
        std::cout << "未做任何修改\n\n";
    }

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
