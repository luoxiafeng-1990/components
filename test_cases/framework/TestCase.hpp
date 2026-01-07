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
    
    /**
     * 打印详细帮助信息（可选实现）
     * 当参数不足或无效时调用
     */
    virtual void printHelp() const {
        // 默认不打印额外帮助
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

/**
 * 多参数函数式测试用例适配器
 * 
 * 支持接收多个命令行参数的测试函数
 */
class MultiArgFunctionTestCase : public TestCase {
public:
    using MultiArgTestFunction = std::function<int(const std::vector<std::string>&)>;
    using HelpFunction = std::function<void()>;
    
    MultiArgFunctionTestCase(const std::string& name, 
                             const std::string& description,
                             const std::string& usage,
                             MultiArgTestFunction func,
                             HelpFunction help_func = nullptr)
        : name_(name), description_(description), usage_(usage), func_(func), help_func_(help_func) {}
    
    int run(const std::vector<std::string>& args) override {
        return func_(args);
    }
    
    const std::string& getName() const override {
        return name_;
    }
    
    const std::string& getDescription() const override {
        return description_;
    }
    
    std::string getUsage() const override {
        return usage_;
    }
    
    void printHelp() const override {
        if (help_func_) {
            help_func_();
        }
    }
    
private:
    std::string name_;
    std::string description_;
    std::string usage_;
    MultiArgTestFunction func_;
    HelpFunction help_func_;
};

} // namespace TestFramework

#endif // TEST_CASE_HPP

