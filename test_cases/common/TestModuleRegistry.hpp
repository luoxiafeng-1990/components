/**
 * @file TestModuleRegistry.hpp
 * @brief 测试模块注册表
 * 
 * 管理所有测试模块的注册和查找，支持：
 * - 模块注册
 * - 模块查找
 * - 全局帮助信息
 * 
 * @version 3.1
 */

#ifndef TEST_MODULE_REGISTRY_HPP
#define TEST_MODULE_REGISTRY_HPP

#include "ITestModule.hpp"
#include <map>
#include <string>
#include <vector>
#include <iostream>

namespace test {
namespace common {

/**
 * @brief 测试模块注册表（单例）
 * 
 * 使用示例：
 * @code
 * auto& registry = TestModuleRegistry::getInstance();
 * 
 * // 注册模块
 * registry.registerModule(std::make_shared<VdecTestSuite>());
 * registry.registerModule(std::make_shared<PPTestSuite>());
 * 
 * // 获取模块
 * auto module = registry.getModule("vdec");
 * if (module) {
 *     module->run(argc, argv);
 * }
 * 
 * // 打印全局帮助
 * registry.printGlobalHelp("./test");
 * @endcode
 */
class TestModuleRegistry {
public:
    /**
     * @brief 获取单例实例
     */
    static TestModuleRegistry& getInstance() {
        static TestModuleRegistry instance;
        return instance;
    }
    
    /**
     * @brief 注册测试模块
     * 
     * @param module 模块指针
     * @return true 注册成功，false 模块已存在
     */
    bool registerModule(ITestModulePtr module) {
        if (!module) return false;
        
        std::string name = module->getName();
        if (modules_.find(name) != modules_.end()) {
            return false; // 模块已存在
        }
        
        modules_[name] = module;
        module_order_.push_back(name);
        return true;
    }
    
    /**
     * @brief 获取测试模块
     * 
     * @param name 模块名称
     * @return 模块指针，未找到返回 nullptr
     */
    ITestModulePtr getModule(const std::string& name) const {
        auto it = modules_.find(name);
        if (it != modules_.end()) {
            return it->second;
        }
        return nullptr;
    }
    
    /**
     * @brief 检查模块是否存在
     */
    bool hasModule(const std::string& name) const {
        return modules_.find(name) != modules_.end();
    }
    
    /**
     * @brief 获取所有模块名称（按注册顺序）
     */
    std::vector<std::string> getModuleNames() const {
        return module_order_;
    }
    
    /**
     * @brief 获取模块数量
     */
    size_t getModuleCount() const {
        return modules_.size();
    }
    
    /**
     * @brief 打印全局帮助信息
     * 
     * @param prog_name 程序名称（用于显示用法）
     */
    void printGlobalHelp(const std::string& prog_name) const {
        std::cout << "\n";
        std::cout << "Usage: " << prog_name << " <module> [options]\n";
        std::cout << "       " << prog_name << " <module> -h        (show module help)\n";
        std::cout << "       " << prog_name << " -h|--help          (show this help)\n";
        std::cout << "       " << prog_name << " -l|--list          (list all modules)\n";
        std::cout << "\n";
        
        std::cout << "Available modules:\n";
        std::cout << "────────────────────────────────────────────────────────\n";
        
        for (const auto& name : module_order_) {
            auto it = modules_.find(name);
            if (it != modules_.end()) {
                std::cout << "  " << name;
                // 对齐描述
                int padding = 12 - static_cast<int>(name.length());
                for (int i = 0; i < padding; i++) std::cout << " ";
                std::cout << it->second->getDescription() << "\n";
            }
        }
        
        std::cout << "────────────────────────────────────────────────────────\n";
        std::cout << "\n";
        std::cout << "Examples:\n";
        std::cout << "  " << prog_name << " vdec --file video.mp4\n";
        std::cout << "  " << prog_name << " pp --format argb888 --input video.mp4\n";
        std::cout << "  " << prog_name << " vdec -h\n";
        std::cout << "\n";
    }
    
    /**
     * @brief 列出所有模块
     */
    void listModules() const {
        std::cout << "\nAvailable modules:\n";
        for (const auto& name : module_order_) {
            auto it = modules_.find(name);
            if (it != modules_.end()) {
                std::cout << "  - " << name << ": " 
                          << it->second->getDescription() << "\n";
            }
        }
        std::cout << "\n";
    }

private:
    TestModuleRegistry() = default;
    ~TestModuleRegistry() = default;
    
    // 禁止复制
    TestModuleRegistry(const TestModuleRegistry&) = delete;
    TestModuleRegistry& operator=(const TestModuleRegistry&) = delete;
    
    std::map<std::string, ITestModulePtr> modules_;
    std::vector<std::string> module_order_;  // 保持注册顺序
};

} // namespace common
} // namespace test

#endif // TEST_MODULE_REGISTRY_HPP
