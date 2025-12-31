/**
 * TestCase.hpp - 测试用例基类接口
 * 
 * 定义统一的测试用例接口，支持函数式测试和类式测试
 */

#ifndef TEST_CASE_HPP
#define TEST_CASE_HPP

#include <string>
#include <vector>
#include <functional>

namespace TestFramework {

/**
 * 测试用例接口
 * 
 * 所有测试用例必须实现此接口，或使用函数适配器
 */
class TestCase {
public:
    virtual ~TestCase() = default;
    
    /**
     * 运行测试用例
     * @param args 命令行参数（去除测试名称后的剩余参数）
     * @return 测试结果，0表示成功，非0表示失败
     */
    virtual int run(const std::vector<std::string>& args) = 0;
    
    /**
     * 获取测试用例名称
     */
    virtual const std::string& getName() const = 0;
    
    /**
     * 获取测试用例描述
     */
    virtual const std::string& getDescription() const = 0;
    
    /**
     * 获取参数要求说明
     */
    virtual std::string getUsage() const {
        return "";
    }
};

/**
 * 函数式测试用例适配器
 * 
 * 将函数指针包装为 TestCase 接口
 */
class FunctionTestCase : public TestCase {
public:
    using TestFunction = std::function<int(const char*)>;
    
    FunctionTestCase(const std::string& name, 
                     const std::string& description,
                     TestFunction func)
        : name_(name), description_(description), func_(func) {}
    
    int run(const std::vector<std::string>& args) override {
        if (args.empty()) {
            return func_(nullptr);
        }
        return func_(args[0].c_str());
    }
    
    const std::string& getName() const override {
        return name_;
    }
    
    const std::string& getDescription() const override {
        return description_;
    }
    
private:
    std::string name_;
    std::string description_;
    TestFunction func_;
};

} // namespace TestFramework

#endif // TEST_CASE_HPP

