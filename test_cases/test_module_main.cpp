/**
 * @file test_module_main.cpp
 * @brief 模块化测试入口
 * 
 * 提供模块化的测试框架入口，支持：
 * - ./test <module> [options] - 运行指定模块
 * - ./test -h - 显示全局帮助
 * - ./test <module> -h - 显示模块帮助
 * 
 * 编译：
 * 此文件编译为独立的可执行文件，与 Google Test 入口分离。
 * 
 * @version 3.1
 */

#include "common/ITestModule.hpp"
#include "common/TestModuleRegistry.hpp"
#include "vdec/VdecTestSuite.hpp"
#include "pp/PPTestSuite.hpp"
#include "record/RecordTestSuite.hpp"
#include "writer/WriterTestSuite.hpp"
#include "opencv/OpencvTestSuite.hpp"
#include "taopencv/TaOpencvTestSuite.hpp"
#include "common/Logger.hpp"

#include <iostream>
#include <cstring>

// 初始化日志（使用与组件相同的宏）
#ifndef INIT_LOGGER
#define INIT_LOGGER() \
    do { \
        auto& logger = Logger::getInstance(); \
        logger.init(spdlog::level::info, true, ""); \
    } while(0)
#endif

/**
 * @brief 注册所有测试模块
 */
static void registerModules() {
    auto& registry = test::common::TestModuleRegistry::getInstance();
    
    // 注册 VDEC 模块（视频解码测试）
    registry.registerModule(std::make_shared<test::vdec::VdecTestSuite>());
    
    // 注册 PP 模块（后处理格式测试）
    registry.registerModule(std::make_shared<test::pp::PPTestSuite>());
    
    // 注册 Record 模块（录制测试）
    registry.registerModule(std::make_shared<test::record::RecordTestSuite>());
    
    // 注册 Writer 模块（BufferWriter 格式测试）
    registry.registerModule(std::make_shared<test::writer::WriterTestSuite>());

    registry.registerModule(std::make_shared<test::opencv::OpencvTestSuite>());

    registry.registerModule(std::make_shared<test::taopencv::TaOpencvTestSuite>());
    
    // TODO: 注册更多模块
    // registry.registerModule(std::make_shared<test::venc::VencTestSuite>());
}

/**
 * @brief 主函数
 */
int main(int argc, char* argv[]) {
    // 初始化日志
    INIT_LOGGER();
    
    // 注册模块
    registerModules();
    
    auto& registry = test::common::TestModuleRegistry::getInstance();
    
    // 无参数 → 显示帮助
    if (argc < 2) {
        registry.printGlobalHelp(argv[0]);
        return 0;
    }
    
    std::string first_arg = argv[1];
    
    // -h 或 --help → 显示全局帮助
    if (first_arg == "-h" || first_arg == "--help") {
        registry.printGlobalHelp(argv[0]);
        return 0;
    }
    
    // -l 或 --list → 列出所有模块
    if (first_arg == "-l" || first_arg == "--list") {
        registry.listModules();
        return 0;
    }
    
    // 获取模块
    auto module = registry.getModule(first_arg);
    
    if (!module) {
        std::cerr << "Error: Unknown module '" << first_arg << "'\n\n";
        registry.printGlobalHelp(argv[0]);
        return 1;
    }
    
    // 将剩余参数传给模块
    // argv[0] = 程序名
    // argv[1] = 模块名（已处理）
    // argv[2...] = 模块参数
    //
    // 传给模块的参数：
    // argc - 1, argv + 1
    // 这样 module 看到的 argv[0] 就是模块名（如 "vdec"）
    
    return module->run(argc - 1, argv + 1);
}
