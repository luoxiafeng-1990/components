/**
 * TestRegistry.hpp - 测试用例注册表（单例模式）
 * 
 * 自动注册和管理所有测试用例，提供统一的查找和运行接口
 */

#ifndef TEST_REGISTRY_HPP
#define TEST_REGISTRY_HPP

#include "TestCase.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <iostream>

namespace TestFramework {

class TestRegistry {
public:
    /**
     * 获取单例实例
     */
    static TestRegistry& getInstance() {
        static TestRegistry instance;
        return instance;
    }
    
    /**
     * 注册测试用例
     * @param name 测试用例名称（命令行使用的名称）
     * @param description 测试用例描述
     * @param test_case 测试用例对象（智能指针，自动管理生命周期）
     */
    void registerTest(const std::string& name,
                      const std::string& description,
                      std::shared_ptr<TestCase> test_case) {
        tests_[name] = test_case;
        descriptions_[name] = description;
    }
    
    /**
     * 根据名称查找测试用例
     * @param name 测试用例名称
     * @return 测试用例对象，如果不存在返回 nullptr
     */
    std::shared_ptr<TestCase> findTest(const std::string& name) const {
        auto it = tests_.find(name);
        if (it != tests_.end()) {
            return it->second;
        }
        return nullptr;
    }
    
    /**
     * 运行指定的测试用例
     * @param name 测试用例名称
     * @param args 测试参数
     * @return 测试结果，0表示成功，非0表示失败
     */
    int runTest(const std::string& name, const std::vector<std::string>& args) {
        auto test = findTest(name);
        if (!test) {
            std::cerr << "Error: Test case '" << name << "' not found" << std::endl;
            return 1;
        }
        return test->run(args);
    }
    
    /**
     * 列出所有注册的测试用例
     */
    void listTests() const {
        std::cout << "\nAvailable test cases:\n" << std::endl;
        for (const auto& pair : tests_) {
            std::cout << "  " << pair.first;
            auto desc_it = descriptions_.find(pair.first);
            if (desc_it != descriptions_.end() && !desc_it->second.empty()) {
                std::cout << " - " << desc_it->second;
            }
            std::cout << std::endl;
        }
        std::cout << std::endl;
    }
    
    /**
     * 获取所有测试用例名称
     */
    std::vector<std::string> getAllTestNames() const {
        std::vector<std::string> names;
        for (const auto& pair : tests_) {
            names.push_back(pair.first);
        }
        return names;
    }
    
    /**
     * 检查测试用例是否存在
     */
    bool hasTest(const std::string& name) const {
        return tests_.find(name) != tests_.end();
    }
    
    /**
     * 获取测试用例数量
     */
    size_t getTestCount() const {
        return tests_.size();
    }

private:
    TestRegistry() = default;
    ~TestRegistry() = default;
    TestRegistry(const TestRegistry&) = delete;
    TestRegistry& operator=(const TestRegistry&) = delete;
    
    std::map<std::string, std::shared_ptr<TestCase>> tests_;
    std::map<std::string, std::string> descriptions_;
};

} // namespace TestFramework

#endif // TEST_REGISTRY_HPP

