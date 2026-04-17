#ifndef WORKER_CONFIG_CONNECTOR_HPP
#define WORKER_CONFIG_CONNECTOR_HPP

#include <string>
#include <vector>
#include <map>
#include <memory>

// 前向声明
class IEncodedPacketSource;

/**
 * @brief Connector - 连接器类
 * 
 * 核心职责：
 * - 定义生产者-消费者之间的映射规则（1:1, 1:N, N:1, N:M）
 * - 为每个消费者分配应该绑定的生产者 BufferPool
 * - 不直接处理数据，只提供路由配置
 * 
 * 设计原则：
 * - 简单：单一类，通过 Mode 枚举选择模式
 * - 必要字段：mode, producer_names, consumer_names
 * - 核心方法：getProducerNameForConsumer()
 * 
 * ⭐ v2.18 新增：
 * - 支持共享 PacketSource（ONE_TO_MANY 模式）
 * - 存储共享实例，防止被销毁
 * 
 * ⭐ v2.20：从 Connector.hpp 移动到 WorkerConfig.hpp（统一配置管理）
 * 
 * ⭐ v2.21：重构为使用名字而非索引，提高一致性和可读性
 * 
 * ⭐ v3.4：拆分到独立头文件 config/Connector.hpp
 */
class Connector {
public:
    enum class Mode {
        ONE_TO_ONE,      // 1:1 映射
        ONE_TO_MANY,     // 1:N 映射（广播模式）
        MANY_TO_ONE,     // N:1 映射（合并模式）
        MANY_TO_MANY     // N:M 映射（轮询策略）
    };
    
    /**
     * @brief 构造函数
     * @param mode 连接器模式
     * @param producer_names 生产者名称列表
     * @param consumer_names 消费者名称列表
     */
    Connector(Mode mode,
              const std::vector<std::string>& producer_names,
              const std::vector<std::string>& consumer_names);
    
    /**
     * @brief 获取消费者对应的生产者名称
     * @param consumer_name 消费者名称
     * @return 生产者名称，空字符串表示没有对应的生产者
     */
    std::string getProducerNameForConsumer(const std::string& consumer_name) const;
    
    /**
     * @brief 检查是否包含指定生产者
     * @param producer_name 生产者名称
     * @return true 如果包含该生产者，false 否则
     */
    bool containsProducer(const std::string& producer_name) const;
    
    /**
     * @brief 检查是否包含指定消费者
     * @param consumer_name 消费者名称
     * @return true 如果包含该消费者，false 否则
     */
    bool containsConsumer(const std::string& consumer_name) const;
    
    // 访问器
    Mode getMode() const;
    const std::vector<std::string>& getProducerNames() const;
    const std::vector<std::string>& getConsumerNames() const;
    
    // ⭐ v2.18 新增：设置共享的 EncodedPacketSource（按生产者名称）
    void setSharedSource(const std::string& producer_name, std::shared_ptr<class IEncodedPacketSource> source);
    std::shared_ptr<class IEncodedPacketSource> getSharedSource(const std::string& producer_name) const;

private:
    Mode mode_;
    std::vector<std::string> producer_names_;
    std::vector<std::string> consumer_names_;
    
    // ⭐ v2.18 新增：共享的 EncodedPacketSource（按生产者名称索引）
    std::map<std::string, std::shared_ptr<class IEncodedPacketSource>> shared_sources_;
};

#endif // WORKER_CONFIG_CONNECTOR_HPP
