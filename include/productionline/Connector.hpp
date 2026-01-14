#pragma once

#include <vector>
#include <string>
#include <memory>

// 前向声明
class IPacketSource;

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
 * - 必要字段：mode, producer_indices, consumer_indices
 * - 核心方法：getProducerIndexForConsumer()
 * 
 * ⭐ v2.18 新增：
 * - 支持共享 PacketSource（ONE_TO_MANY 模式）
 * - 存储共享实例，防止被销毁
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
     * @param producer_indices 生产者索引列表（在 Group 的 producers 数组中的位置）
     * @param consumer_indices 消费者索引列表（在 Group 的 consumers 数组中的位置）
     */
    Connector(Mode mode,
              const std::vector<size_t>& producer_indices,
              const std::vector<size_t>& consumer_indices);
    
    /**
     * @brief 获取消费者对应的生产者索引
     * @param consumer_index 消费者在 consumer_indices 中的索引
     * @return 生产者索引，-1 表示没有对应的生产者
     */
    int getProducerIndexForConsumer(size_t consumer_index) const;
    
    // 访问器
    Mode getMode() const { return mode_; }
    const std::vector<size_t>& getProducerIndices() const { return producer_indices_; }
    const std::vector<size_t>& getConsumerIndices() const { return consumer_indices_; }
    
    // ⭐ v2.18 新增：设置共享的 PacketSource
    /**
     * @brief 设置共享的 PacketSource（用于 ONE_TO_MANY 模式）
     * @param source 共享的 PacketSource 实例
     * 
     * 功能：
     * - Connector 持有共享实例，防止被销毁
     * - 仅在 ONE_TO_MANY 模式下使用
     */
    void setSharedSource(std::shared_ptr<IPacketSource> source) {
        shared_source_ = source;
    }
    
    /**
     * @brief 获取共享的 PacketSource
     * @return 共享实例（如果没有则返回 nullptr）
     */
    std::shared_ptr<IPacketSource> getSharedSource() const {
        return shared_source_;
    }
    
private:
    Mode mode_;
    std::vector<size_t> producer_indices_;
    std::vector<size_t> consumer_indices_;
    std::vector<int> mapping_;  // consumer_index -> producer_index
    
    // ⭐ v2.18 新增：共享的 PacketSource（仅 ONE_TO_MANY 模式使用）
    std::shared_ptr<IPacketSource> shared_source_;
    
    void computeMapping();  // 根据 mode 计算映射关系
};
