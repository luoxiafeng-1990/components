/**
 * @file ITestModule.hpp
 * @brief 测试模块接口定义
 * 
 * 定义所有测试模块必须实现的接口，支持：
 * - 模块化测试组织
 * - 统一的命令行接口
 * - 全局和模块级帮助信息
 * 
 * @version 3.1
 */

#ifndef ITEST_MODULE_HPP
#define ITEST_MODULE_HPP

#include <string>
#include <vector>
#include <memory>

namespace test {
namespace common {

/**
 * @brief 测试模块接口
 * 
 * 所有测试模块（如 VdecTestSuite, PPTestSuite）必须实现此接口。
 * 
 * 使用方式：
 * - ./test <module> [options] - 运行指定模块
 * - ./test <module> -h       - 显示模块帮助
 * - ./test -h                - 显示全局帮助（列出所有模块）
 */
class ITestModule {
public:
    virtual ~ITestModule() = default;
    
    /**
     * @brief 获取模块名称（命令行标识）
     * 
     * @return 模块名称，如 "vdec", "pp", "venc"
     */
    virtual std::string getName() const = 0;
    
    /**
     * @brief 获取模块描述（显示在帮助信息中）
     * 
     * @return 模块描述，如 "视频解码测试"
     */
    virtual std::string getDescription() const = 0;
    
    /**
     * @brief 运行模块
     * 
     * @param argc 参数数量（不包含模块名）
     * @param argv 参数数组（argv[0] 是模块名后的第一个参数）
     * @return 退出码，0 表示成功
     * 
     * @note 参数已经去除了程序名和模块名，例如：
     *       原始命令: ./test vdec --file video.mp4 --decoder h264
     *       传入 run: argc=4, argv=["--file", "video.mp4", "--decoder", "h264"]
     */
    virtual int run(int argc, char* argv[]) = 0;
    
    /**
     * @brief 打印模块帮助信息
     * 
     * 显示模块的使用方法、参数说明、可用测试项等。
     */
    virtual void printHelp() const = 0;
    
    /**
     * @brief 列出所有测试项
     * 
     * 显示模块支持的所有预定义测试用例。
     */
    virtual void listTests() const = 0;
    
    /**
     * @brief 获取支持的测试项列表
     * 
     * @return 测试项名称列表
     */
    virtual std::vector<std::string> getTestNames() const {
        return {}; // 默认返回空列表
    }
};

/**
 * @brief 测试模块指针类型
 */
using ITestModulePtr = std::shared_ptr<ITestModule>;

} // namespace common
} // namespace test

#endif // ITEST_MODULE_HPP
