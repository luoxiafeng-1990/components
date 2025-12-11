/**
 * TestMacros.hpp - 测试框架宏定义
 * 
 * 提供简化的宏接口，用于注册测试用例和启动测试框架
 */

#ifndef TEST_MACROS_HPP
#define TEST_MACROS_HPP

#include "TestCase.hpp"
#include "TestRegistry.hpp"
#include <getopt.h>
#include <cstring>
#include <iostream>

namespace TestFramework {

/**
 * 命令行解析器
 */
class CommandLineParser {
public:
    struct Options {
        std::string test_name;
        std::vector<std::string> test_args;
        bool show_help = false;
        bool list_tests = false;
    };
    
    static Options parse(int argc, char* argv[]) {
        Options opts;
        
        // 定义长选项
        static struct option long_options[] = {
            {"help",    no_argument,       0, 'h'},
            {"list",    no_argument,       0, 'l'},
            {"mode",    required_argument, 0, 'm'},
            {0,         0,                 0,  0 }
        };
        
        int opt;
        int option_index = 0;
        
        while ((opt = getopt_long(argc, argv, "hlm:", long_options, &option_index)) != -1) {
            switch (opt) {
                case 'h':
                    opts.show_help = true;
                    break;
                
                case 'l':
                    opts.list_tests = true;
                    break;
                
                case 'm':
                    opts.test_name = optarg;
                    break;
                
                case '?':
                    // getopt_long 已经打印了错误信息
                    opts.show_help = true;
                    break;
                
                default:
                    opts.show_help = true;
                    break;
            }
        }
        
        // 如果没有通过 -m 指定，尝试从第一个非选项参数获取
        if (opts.test_name.empty() && optind < argc) {
            // 检查是否是测试名称（不是文件路径）
            std::string first_arg = argv[optind];
            if (TestRegistry::getInstance().hasTest(first_arg)) {
                opts.test_name = first_arg;
                optind++;
            }
        }
        
        // 收集剩余参数作为测试参数
        for (int i = optind; i < argc; i++) {
            opts.test_args.push_back(argv[i]);
        }
        
        return opts;
    }
    
    static void printUsage(const char* prog_name) {
        auto& registry = TestRegistry::getInstance();
        
        std::cout << "Usage: " << prog_name << " [options] [test_name] [test_args...]\n\n";
        std::cout << "Options:\n";
        std::cout << "  -h, --help          Show this help message\n";
        std::cout << "  -l, --list          List all available test cases\n";
        std::cout << "  -m, --mode <name>   Run test case by name\n";
        std::cout << "\n";
        
        if (registry.getTestCount() > 0) {
            std::cout << "Available test cases:\n";
            registry.listTests();
        }
        
        std::cout << "Examples:\n";
        if (registry.getTestCount() > 0) {
            auto names = registry.getAllTestNames();
            if (!names.empty()) {
                std::cout << "  " << prog_name << " -m " << names[0] << " <test_file>\n";
                std::cout << "  " << prog_name << " " << names[0] << " <test_file>\n";
            }
        }
        std::cout << "  " << prog_name << " -l\n";
        std::cout << "  " << prog_name << " --help\n";
        std::cout << "\n";
        std::cout << "Note:\n";
        std::cout << "  - If no test name is specified, the first test case will be used as default\n";
        std::cout << "  - Test arguments are passed to the test case function\n";
    }
};

} // namespace TestFramework

/**
 * 注册函数式测试用例
 * 
 * @param name 测试用例名称（标识符，不带引号，如 loop）
 *             宏内部会自动字符串化为 "loop" 用于注册
 * @param description 测试用例描述（显示在帮助信息中）
 * @param func 测试函数指针（签名：int func(const char* arg)）
 * 
 * 使用示例：
 *   REGISTER_TEST(loop, "4-frame loop display", test_4frame_loop);
 */
#define REGISTER_TEST(name, description, func) \
    namespace { \
        struct TestRegistrar_##name { \
            TestRegistrar_##name() { \
                auto test_case = std::make_shared<TestFramework::FunctionTestCase>( \
                    #name, \
                    description, \
                    [](const char* arg) { return func(arg); } \
                ); \
                TestFramework::TestRegistry::getInstance().registerTest( \
                    #name, \
                    description, \
                    test_case \
                ); \
            } \
        }; \
        static TestRegistrar_##name g_test_registrar_##name; \
    }

/**
 * 测试框架主函数入口
 * 
 * 自动解析命令行参数，运行指定的测试用例
 * 
 * 使用示例：
 *   int main(int argc, char* argv[]) {
 *       INIT_LOGGER();  // 无需配置文件，使用编程式配置
 *       // ... 其他初始化代码 ...
 *       TEST_MAIN(argc, argv);
 *   }
 */
#define TEST_MAIN(argc, argv) \
    do { \
        using namespace TestFramework; \
        auto opts = CommandLineParser::parse(argc, argv); \
        \
        if (opts.show_help) { \
            CommandLineParser::printUsage(argv[0]); \
            return 0; \
        } \
        \
        if (opts.list_tests) { \
            TestRegistry::getInstance().listTests(); \
            return 0; \
        } \
        \
        auto& registry = TestRegistry::getInstance(); \
        \
        if (registry.getTestCount() == 0) { \
            std::cerr << "Error: No test cases registered" << std::endl; \
            return 1; \
        } \
        \
        std::string test_name = opts.test_name; \
        \
        /* 如果没有指定测试名称，使用第一个注册的测试作为默认值 */ \
        if (test_name.empty()) { \
            auto names = registry.getAllTestNames(); \
            if (!names.empty()) { \
                test_name = names[0]; \
            } \
        } \
        \
        if (test_name.empty()) { \
            std::cerr << "Error: No test case specified" << std::endl; \
            CommandLineParser::printUsage(argv[0]); \
            return 1; \
        } \
        \
        if (!registry.hasTest(test_name)) { \
            std::cerr << "Error: Test case '" << test_name << "' not found" << std::endl; \
            std::cerr << "\nAvailable test cases:" << std::endl; \
            registry.listTests(); \
            return 1; \
        } \
        \
        if (opts.test_args.empty()) { \
            std::cerr << "Error: Missing test argument (e.g., video file path)" << std::endl; \
            std::cerr << "Usage: " << argv[0] << " -m " << test_name << " <test_file>" << std::endl; \
            return 1; \
        } \
        \
        return registry.runTest(test_name, opts.test_args); \
    } while (0)

#endif // TEST_MACROS_HPP

