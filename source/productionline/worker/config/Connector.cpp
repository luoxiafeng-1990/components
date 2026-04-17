#include "productionline/worker/config/Connector.hpp"
#include <stdexcept>

// ============================================================
// Connector 类实现（v2.20：从 Connector.cpp 移动）
// ============================================================

Connector::Connector(Mode mode,
                     const std::vector<std::string>& producer_names,
                     const std::vector<std::string>& consumer_names)
    : mode_(mode)
    , producer_names_(producer_names)
    , consumer_names_(consumer_names)
{
    if (producer_names_.empty()) {
        throw std::invalid_argument("Connector: producer_names cannot be empty");
    }
    if (consumer_names_.empty()) {
        throw std::invalid_argument("Connector: consumer_names cannot be empty");
    }
    
    // 验证模式约束
    switch (mode_) {
        case Mode::ONE_TO_ONE: {
            if (producer_names_.size() != consumer_names_.size()) {
                throw std::invalid_argument("Connector ONE_TO_ONE: producer_names.size() must equal consumer_names.size()");
            }
            break;
        }
        case Mode::ONE_TO_MANY: {
            if (producer_names_.size() != 1) {
                throw std::invalid_argument("Connector ONE_TO_MANY: producer_names.size() must be 1");
            }
            break;
        }
        case Mode::MANY_TO_ONE: {
            if (consumer_names_.size() != 1) {
                throw std::invalid_argument("Connector MANY_TO_ONE: consumer_names.size() must be 1");
            }
            break;
        }
        case Mode::MANY_TO_MANY: {
            // 无特殊约束
            break;
        }
    }
}

std::string Connector::getProducerNameForConsumer(const std::string& consumer_name) const {
    // 查找 consumer_name 在 consumer_names_ 中的位置
    size_t consumer_idx = SIZE_MAX;
    for (size_t i = 0; i < consumer_names_.size(); i++) {
        if (consumer_names_[i] == consumer_name) {
            consumer_idx = i;
            break;
        }
    }
    
    if (consumer_idx == SIZE_MAX) {
        return "";  // 消费者不存在
    }
    
    // 根据模式动态计算对应的生产者
    switch (mode_) {
        case Mode::ONE_TO_ONE: {
            // 1:1 映射：consumer_names[i] -> producer_names[i]
            return producer_names_[consumer_idx];
        }
        
        case Mode::ONE_TO_MANY: {
            // 1:N 映射：所有消费者都绑定到同一个生产者（第一个）
            return producer_names_[0];
        }
        
        case Mode::MANY_TO_ONE: {
            // N:1 映射：第一个消费者绑定到第一个生产者
            return producer_names_[0];
        }
        
        case Mode::MANY_TO_MANY: {
            // N:M 映射：轮询策略
            // consumer_names[i] 绑定到 producer_names[i % producer_names_.size()]
            size_t producer_idx = consumer_idx % producer_names_.size();
            return producer_names_[producer_idx];
        }
    }
    
    return "";  // 不应该到达这里
}

// ============================================================
// Connector 访问器实现
// ============================================================

Connector::Mode Connector::getMode() const {
    return mode_;
}

const std::vector<std::string>& Connector::getProducerNames() const {
    return producer_names_;
}

const std::vector<std::string>& Connector::getConsumerNames() const {
    return consumer_names_;
}

bool Connector::containsProducer(const std::string& producer_name) const {
    for (const auto& name : producer_names_) {
        if (name == producer_name) {
            return true;
        }
    }
    return false;
}

bool Connector::containsConsumer(const std::string& consumer_name) const {
    for (const auto& name : consumer_names_) {
        if (name == consumer_name) {
            return true;
        }
    }
    return false;
}

void Connector::setSharedSource(const std::string& producer_name, std::shared_ptr<class IEncodedPacketSource> source) {
    shared_sources_[producer_name] = source;
}

std::shared_ptr<class IEncodedPacketSource> Connector::getSharedSource(const std::string& producer_name) const {
    auto it = shared_sources_.find(producer_name);
    if (it != shared_sources_.end()) {
        return it->second;
    }
    return nullptr;
}
