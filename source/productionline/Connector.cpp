#include "productionline/Connector.hpp"
#include <stdexcept>

Connector::Connector(Mode mode,
                     const std::vector<size_t>& producer_indices,
                     const std::vector<size_t>& consumer_indices)
    : mode_(mode)
    , producer_indices_(producer_indices)
    , consumer_indices_(consumer_indices)
{
    if (producer_indices_.empty()) {
        throw std::invalid_argument("Connector: producer_indices cannot be empty");
    }
    if (consumer_indices_.empty()) {
        throw std::invalid_argument("Connector: consumer_indices cannot be empty");
    }
    
    computeMapping();
}

void Connector::computeMapping() {
    mapping_.clear();
    mapping_.resize(consumer_indices_.size(), -1);
    
    switch (mode_) {
        case Mode::ONE_TO_ONE: {
            // 1:1 映射：producer_indices[i] -> consumer_indices[i]
            if (producer_indices_.size() != consumer_indices_.size()) {
                throw std::invalid_argument("Connector ONE_TO_ONE: producer_indices.size() must equal consumer_indices.size()");
            }
            for (size_t i = 0; i < consumer_indices_.size(); i++) {
                mapping_[i] = static_cast<int>(i);  // consumer_index i 对应 producer_index i
            }
            break;
        }
        
        case Mode::ONE_TO_MANY: {
            // 1:N 映射：所有消费者都绑定到同一个生产者（索引0）
            if (producer_indices_.size() != 1) {
                throw std::invalid_argument("Connector ONE_TO_MANY: producer_indices.size() must be 1");
            }
            for (size_t i = 0; i < consumer_indices_.size(); i++) {
                mapping_[i] = 0;  // 所有消费者都绑定到生产者索引0
            }
            break;
        }
        
        case Mode::MANY_TO_ONE: {
            // N:1 映射：所有生产者轮询绑定到同一个消费者
            if (consumer_indices_.size() != 1) {
                throw std::invalid_argument("Connector MANY_TO_ONE: consumer_indices.size() must be 1");
            }
            // 使用轮询策略：第一个消费者绑定到第一个生产者
            // 注意：在实际使用中，可能需要更复杂的轮询逻辑
            mapping_[0] = 0;  // 消费者索引0绑定到生产者索引0
            break;
        }
        
        case Mode::MANY_TO_MANY: {
            // N:M 映射：轮询策略
            // consumer_index i 绑定到 producer_index (i % producer_indices_.size())
            for (size_t i = 0; i < consumer_indices_.size(); i++) {
                mapping_[i] = static_cast<int>(i % producer_indices_.size());
            }
            break;
        }
    }
}

int Connector::getProducerIndexForConsumer(size_t consumer_index) const {
    if (consumer_index >= consumer_indices_.size()) {
        return -1;
    }
    
    int producer_idx_in_mapping = mapping_[consumer_index];
    if (producer_idx_in_mapping < 0) {
        return -1;
    }
    
    // producer_idx_in_mapping 是 producer_indices_ 数组中的索引
    // 需要返回实际的 producer_indices_[producer_idx_in_mapping]
    if (static_cast<size_t>(producer_idx_in_mapping) >= producer_indices_.size()) {
        return -1;
    }
    
    return static_cast<int>(producer_indices_[producer_idx_in_mapping]);
}
