# MultiWorkerProductionLine 完整设计方案

本文档记录了 MultiWorkerProductionLine 的完整设计方案（除 `start()` 和 `groupThreadFunc()` 外）。

---

## 一、Connector 类设计

### 1.1 类定义

```cpp
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
    Mode getMode() const;
    const std::vector<size_t>& getProducerIndices() const;
    const std::vector<size_t>& getConsumerIndices() const;
    
private:
    Mode mode_;
    std::vector<size_t> producer_indices_;
    std::vector<size_t> consumer_indices_;
    std::vector<int> mapping_;  // consumer_index -> producer_index
    
    void computeMapping();  // 根据 mode 计算映射关系
};
```

### 1.2 映射规则

- **ONE_TO_ONE**: `consumer[i] -> producer[i]`
- **ONE_TO_MANY**: 所有 `consumer` 都绑定 `producer[0]`
- **MANY_TO_ONE**: `consumer[0]` 绑定 `producer[0]`（简化版）
- **MANY_TO_MANY**: 轮询策略 `consumer[i] -> producer[i % producer_count]`

---

## 二、配置结构设计

### 2.1 配置结构定义

```cpp
struct ProducerConfig {
    std::string producer_id;      // 组内唯一标识
    WorkerConfig worker_config;
};

struct ConsumerConfig {
    std::string consumer_id;      // 组内唯一标识（可选）
    WorkerConfig worker_config;
};

struct ConnectorConfig {
    Connector::Mode mode;
    std::vector<std::string> producer_ids;  // 关联的生产者 ID
    std::vector<std::string> consumer_ids;  // 关联的消费者 ID
};

struct WorkerGroup {
    std::string group_id;
    
    // 多个生产者和消费者
    std::vector<ProducerConfig> producer_configs;
    std::vector<ConsumerConfig> consumer_configs;
    
    // 多个连接器
    std::vector<ConnectorConfig> connector_configs;
    
    // 组级别配置（可选，覆盖全局配置）
    int sync_timeout_ms = -1;
    int max_consecutive_errors = -1;
    bool continue_on_error = false;
};

struct MultiWorkerConfig {
    std::vector<WorkerGroup> groups;
    
    // 全局线程池配置
    int thread_pool_size = 4;
    int max_pending_tasks = 100;
    
    // 全局容错配置（Group 可覆盖）
    int default_sync_timeout_ms = 5000;
    int default_max_consecutive_errors = 10;
    bool default_continue_on_error = false;
};
```

---

## 三、运行时结构设计（简化版）

### 3.1 GroupData 结构

```cpp
struct GroupData {
    std::string group_id;
    
    // 直接存储生产者（使用父类 VideoProductionLine）
    std::vector<std::unique_ptr<VideoProductionLine>> producers;
    std::unordered_map<std::string, VideoProductionLine*> producer_by_id;
    
    // 直接存储消费者（使用 BufferFillingWorkerFacade）
    std::vector<std::shared_ptr<BufferFillingWorkerFacade>> consumers;
    std::unordered_map<std::string, BufferFillingWorkerFacade*> consumer_by_id;
    
    // 连接器列表（直接存储 Connector，不需要 ConnectorRuntime）
    std::vector<std::unique_ptr<Connector>> connectors;
    
    // Group 独立线程
    std::thread group_thread;
    std::atomic<bool> is_running{false};
    
    // Group 级别统计
    std::atomic<int64_t> processed_count{0};
    std::atomic<int64_t> success_count{0};
    std::atomic<int64_t> error_count{0};
    std::atomic<int> consecutive_errors{0};
    
    // Group 配置
    int sync_timeout_ms;
    int max_consecutive_errors;
    bool continue_on_error;
};
```

### 3.2 MultiWorkerProductionLine 成员变量

```cpp
class MultiWorkerProductionLine : public VideoProductionLine {
private:
    MultiWorkerConfig config_;
    std::vector<std::unique_ptr<GroupData>> groups_;
    std::unique_ptr<BS::thread_pool<>> thread_pool_;
    Statistics stats_;
    std::string log_prefix_;
    
    // 错误处理（沿用父类设计）
    std::queue<std::string> error_queue_;  // 扩展：错误队列（用于诊断）
};
```

---

## 四、构造函数和析构函数

### 4.1 构造函数

```cpp
/**
 * @brief 构造函数
 * 
 * 调用父类构造函数，初始化 MultiWorker 特有配置
 */
MultiWorkerProductionLine(
    const MultiWorkerConfig& config,
    bool loop = false,
    int thread_count = 1,
    bool enable_monitor = false
) : VideoProductionLine(loop, thread_count, enable_monitor),
    config_(config),
    log_prefix_("[MultiWorkerProductionLine]")
{
    // 父类已经初始化了 loop_, thread_count_, enable_monitor_, running_ 等
    // 这里只需要初始化 MultiWorker 特有的配置
}
```

### 4.2 析构函数

```cpp
/**
 * @brief 析构函数
 * 
 * 调用重写的 stop()，清理所有 Group
 */
~MultiWorkerProductionLine() {
    stop();  // 调用重写的 stop()
    groups_.clear();
    thread_pool_.reset();
}
```

---

## 五、核心接口（重写父类）

### 5.1 start() - 启动（待讨论）

```cpp
/**
 * @brief 启动多Worker生产流水线（重写父类 start()）
 * 
 * 父类 start() 需要 WorkerConfig，这里重写为无参数版本
 * 启动所有 Group 的生产者和消费者
 * 
 * ⚠️ 待讨论：具体实现逻辑
 */
bool start() override;
```

### 5.2 stop() - 停止

```cpp
/**
 * @brief 停止多Worker生产流水线（重写父类 stop()）
 * 
 * 沿用父类逻辑：设置 running_ = false，等待所有线程退出
 * 扩展到多个 Group：停止所有 Group 线程和线程池
 */
void stop() override {
    if (!running_.load()) {
        return;
    }
    
    // 设置停止标志（使用父类的 running_）
    running_.store(false);
    
    // 停止所有 Group 线程
    for (auto& group : groups_) {
        if (group) {
            group->is_running.store(false);
            if (group->group_thread.joinable()) {
                group->group_thread.join();
            }
        }
    }
    
    // 停止线程池（等待所有任务完成）
    if (thread_pool_) {
        thread_pool_->wait();
        thread_pool_.reset();
    }
    
    // 停止所有生产者和消费者
    for (auto& group : groups_) {
        if (!group) continue;
        
        // 停止生产者
        for (auto& producer : group->producers) {
            if (producer) {
                producer->stop();
            }
        }
        
        // 关闭所有消费者
        for (auto& consumer : group->consumers) {
            if (consumer) {
                consumer->close();
            }
        }
    }
}
```

---

## 六、查询接口（三级设计：整体 + Group + Producer/Consumer）

### 6.1 isRunning() - 运行状态查询

#### 整体级别（重写父类）

```cpp
bool isRunning() const override {
    if (!running_.load()) return false;
    // 检查是否有 Group 在运行
    for (const auto& group : groups_) {
        if (group && group->is_running.load()) {
            return true;
        }
    }
    return false;
}
```

#### Group 级别（新增）

```cpp
bool isGroupRunning(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return false;
    }
    return groups_[group_index]->is_running.load();
}
```

#### Producer 级别（新增）

```cpp
bool isProducerRunning(size_t group_index, size_t producer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return false;
    }
    const auto& group = groups_[group_index];
    if (producer_index >= group->producers.size() || !group->producers[producer_index]) {
        return false;
    }
    return group->producers[producer_index]->isRunning();
}
```

#### Consumer 级别（新增）

```cpp
bool isConsumerRunning(size_t group_index, size_t consumer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return false;
    }
    const auto& group = groups_[group_index];
    if (consumer_index >= group->consumers.size() || !group->consumers[consumer_index]) {
        return false;
    }
    return group->consumers[consumer_index]->isOpen();
}
```

---

### 6.2 getProducedFrames() - 已生产帧数

#### 整体级别（重写父类）

```cpp
// 只统计 Producer（向后兼容）
int getProducedFrames() const override {
    return getProducerTotalFrames();
}

// Producer + Consumer 总和
int getTotalProducedFrames() const {
    return getProducerTotalFrames() + getConsumerTotalFrames();
}

// 所有 Producer 的总和
int getProducerTotalFrames() const {
    int total = 0;
    for (size_t g = 0; g < groups_.size(); g++) {
        total += getGroupProducerProducedFrames(g);
    }
    return total;
}

// 所有 Consumer 的总和
int getConsumerTotalFrames() const {
    int total = 0;
    for (size_t g = 0; g < groups_.size(); g++) {
        total += getGroupConsumerProducedFrames(g);
    }
    return total;
}
```

#### Group 级别（新增）

```cpp
int getGroupTotalProducedFrames(size_t group_index) const {
    return getGroupProducerProducedFrames(group_index) + 
           getGroupConsumerProducedFrames(group_index);
}

int getGroupProducerProducedFrames(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    int total = 0;
    for (const auto& producer : group->producers) {
        if (producer) {
            total += producer->getProducedFrames();
        }
    }
    return total;
}

int getGroupConsumerProducedFrames(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    int total = 0;
    for (size_t i = 0; i < group->consumers.size(); i++) {
        total += getConsumerProducedFrames(group_index, i);
    }
    return total;
}
```

#### Producer 级别（新增）

```cpp
int getProducerProducedFrames(size_t group_index, size_t producer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    if (producer_index >= group->producers.size() || !group->producers[producer_index]) {
        return 0;
    }
    return group->producers[producer_index]->getProducedFrames();
}
```

#### Consumer 级别（新增）- 通过 BufferPool 获取统计

```cpp
int getConsumerProducedFrames(size_t group_index, size_t consumer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    if (consumer_index >= group->consumers.size()) {
        return 0;
    }
    
    // 通过 Consumer 的输出 BufferPool 获取统计
    uint64_t consumer_pool_id = getGroupConsumerBufferPoolId(group_index, consumer_index);
    if (consumer_pool_id == 0) {
        return 0;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(consumer_pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        return 0;
    }
    
    // 使用 BufferPool 的统计接口
    return static_cast<int>(pool->getProducedFrames());
}
```

---

### 6.3 getSkippedFrames() - 跳过帧数（类似设计）

#### 整体级别（重写父类）

```cpp
int getSkippedFrames() const override {
    return getProducerTotalSkippedFrames();
}

int getTotalSkippedFrames() const {
    return getProducerTotalSkippedFrames() + getConsumerTotalSkippedFrames();
}

int getProducerTotalSkippedFrames() const {
    int total = 0;
    for (size_t g = 0; g < groups_.size(); g++) {
        total += getGroupProducerSkippedFrames(g);
    }
    return total;
}

int getConsumerTotalSkippedFrames() const {
    int total = 0;
    for (size_t g = 0; g < groups_.size(); g++) {
        total += getGroupConsumerSkippedFrames(g);
    }
    return total;
}
```

#### Group 级别（新增）

```cpp
int getGroupTotalSkippedFrames(size_t group_index) const {
    return getGroupProducerSkippedFrames(group_index) + 
           getGroupConsumerSkippedFrames(group_index);
}

int getGroupProducerSkippedFrames(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    int total = 0;
    for (const auto& producer : group->producers) {
        if (producer) {
            total += producer->getSkippedFrames();
        }
    }
    return total;
}

int getGroupConsumerSkippedFrames(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    int total = 0;
    for (size_t i = 0; i < group->consumers.size(); i++) {
        total += getConsumerSkippedFrames(group_index, i);
    }
    return total;
}
```

#### Producer/Consumer 级别（新增）

```cpp
int getProducerSkippedFrames(size_t group_index, size_t producer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    if (producer_index >= group->producers.size() || !group->producers[producer_index]) {
        return 0;
    }
    return group->producers[producer_index]->getSkippedFrames();
}

int getConsumerSkippedFrames(size_t group_index, size_t consumer_index) const {
    // 通过 Consumer 的输出 BufferPool 获取统计
    uint64_t consumer_pool_id = getGroupConsumerBufferPoolId(group_index, consumer_index);
    if (consumer_pool_id == 0) {
        return 0;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(consumer_pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        return 0;
    }
    
    // 使用 BufferPool 的统计接口
    return static_cast<int>(pool->getSkippedFrames());
}
```

---

### 6.4 getAverageFPS() - 平均 FPS（类似设计）

#### 整体级别（重写父类）

```cpp
double getAverageFPS() const override {
    return getProducerAverageFPS();
}

double getTotalAverageFPS() const {
    // 使用父类的 start_time_ 计算总时长
    auto duration = std::chrono::steady_clock::now() - start_time_;
    double seconds = std::chrono::duration<double>(duration).count();
    if (seconds > 0) {
        return getTotalProducedFrames() / seconds;
    }
    return 0.0;
}

double getProducerAverageFPS() const {
    auto duration = std::chrono::steady_clock::now() - start_time_;
    double seconds = std::chrono::duration<double>(duration).count();
    if (seconds > 0) {
        return getProducerTotalFrames() / seconds;
    }
    return 0.0;
}

double getConsumerAverageFPS() const {
    auto duration = std::chrono::steady_clock::now() - start_time_;
    double seconds = std::chrono::duration<double>(duration).count();
    if (seconds > 0) {
        return getConsumerTotalFrames() / seconds;
    }
    return 0.0;
}
```

#### Group 级别（新增）

```cpp
double getGroupTotalAverageFPS(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0.0;
    }
    const auto& group = groups_[group_index];
    auto duration = std::chrono::steady_clock::now() - start_time_;
    double seconds = std::chrono::duration<double>(duration).count();
    if (seconds > 0) {
        return getGroupTotalProducedFrames(group_index) / seconds;
    }
    return 0.0;
}

double getGroupProducerAverageFPS(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0.0;
    }
    const auto& group = groups_[group_index];
    auto duration = std::chrono::steady_clock::now() - start_time_;
    double seconds = std::chrono::duration<double>(duration).count();
    if (seconds > 0) {
        return getGroupProducerProducedFrames(group_index) / seconds;
    }
    return 0.0;
}

double getGroupConsumerAverageFPS(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0.0;
    }
    const auto& group = groups_[group_index];
    auto duration = std::chrono::steady_clock::now() - start_time_;
    double seconds = std::chrono::duration<double>(duration).count();
    if (seconds > 0) {
        return getGroupConsumerProducedFrames(group_index) / seconds;
    }
    return 0.0;
}
```

#### Producer/Consumer 级别（新增）

```cpp
double getProducerAverageFPS(size_t group_index, size_t producer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0.0;
    }
    const auto& group = groups_[group_index];
    if (producer_index >= group->producers.size() || !group->producers[producer_index]) {
        return 0.0;
    }
    return group->producers[producer_index]->getAverageFPS();
}

double getConsumerAverageFPS(size_t group_index, size_t consumer_index) const {
    // 通过 Consumer 的输出 BufferPool 获取统计
    uint64_t consumer_pool_id = getGroupConsumerBufferPoolId(group_index, consumer_index);
    if (consumer_pool_id == 0) {
        return 0.0;
    }
    
    auto pool_weak = BufferPoolRegistry::getInstance().getPool(consumer_pool_id);
    auto pool = pool_weak.lock();
    if (!pool) {
        return 0.0;
    }
    
    // 使用 BufferPool 的统计接口
    return pool->getAverageProduceFPS();
}
```

---

### 6.5 getWorkingBufferPoolId() - BufferPool ID

#### 整体级别（重写父类）

```cpp
/**
 * @brief 获取工作BufferPool ID（重写父类 getWorkingBufferPoolId()）
 * 
 * 向后兼容：返回第一个生产者的 BufferPool ID
 * 注意：MultiWorker 有多个生产者，建议使用 getGroupProducerBufferPoolId()
 */
uint64_t getWorkingBufferPoolId() const override {
    if (groups_.empty() || !groups_[0]) {
        return 0;
    }
    if (groups_[0]->producers.empty() || !groups_[0]->producers[0]) {
        return 0;
    }
    return groups_[0]->producers[0]->getWorkingBufferPoolId();
}
```

#### Group/Producer 级别（已有）

```cpp
uint64_t getGroupProducerBufferPoolId(size_t group_index, size_t producer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    if (producer_index >= group->producers.size() || !group->producers[producer_index]) {
        return 0;
    }
    return group->producers[producer_index]->getWorkingBufferPoolId();
}
```

#### Group/Consumer 级别（已有）

```cpp
uint64_t getGroupConsumerBufferPoolId(size_t group_index, size_t consumer_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    const auto& group = groups_[group_index];
    if (consumer_index >= group->consumers.size() || !group->consumers[consumer_index]) {
        return 0;
    }
    
    BufferPoolType primary_type = group->consumers[consumer_index]->getPrimaryBufferPoolType();
    return group->consumers[consumer_index]->getOutputBufferPoolId(primary_type);
}
```

---

### 6.6 getWorkerFacade() - Worker Facade

#### 整体级别（重写父类）

```cpp
/**
 * @brief 获取Worker Facade（重写父类 getWorkerFacade()）
 * 
 * 向后兼容：返回第一个生产者的 Worker Facade
 * 注意：MultiWorker 有多个生产者，建议使用 getGroupProducerWorkerFacade()
 */
std::shared_ptr<BufferFillingWorkerFacade> getWorkerFacade() const override {
    if (groups_.empty() || !groups_[0]) {
        return nullptr;
    }
    if (groups_[0]->producers.empty() || !groups_[0]->producers[0]) {
        return nullptr;
    }
    return groups_[0]->producers[0]->getWorkerFacade();
}
```

#### Group/Producer 级别（新增）

```cpp
std::shared_ptr<BufferFillingWorkerFacade> getGroupProducerWorkerFacade(
    size_t group_index, 
    size_t producer_index
) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return nullptr;
    }
    const auto& group = groups_[group_index];
    if (producer_index >= group->producers.size() || !group->producers[producer_index]) {
        return nullptr;
    }
    return group->producers[producer_index]->getWorkerFacade();
}
```

---

### 6.7 getLastError() - 错误信息

#### 整体级别（重写父类）

```cpp
/**
 * @brief 获取最后错误（重写父类 getLastError()）
 * 
 * 沿用父类逻辑：返回 last_error_
 * 扩展到多个 Group：返回最近的错误（可能来自任何 Group）
 */
std::string getLastError() const override {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (!error_queue_.empty()) {
        return error_queue_.back();  // 返回最近的错误
    }
    return last_error_;  // 使用父类的 last_error_
}
```

#### Group 级别（新增）

```cpp
std::string getGroupLastError(size_t group_index) const {
    // 可以从 Group 中维护错误信息，或者从各个 Producer/Consumer 获取
    // 具体实现待定
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return "";
    }
    // TODO: 实现 Group 级别的错误信息获取
    return "";
}
```

#### 获取所有错误历史（新增）

```cpp
std::vector<std::string> getAllErrors() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    std::vector<std::string> errors;
    // 将 error_queue_ 转换为 vector
    std::queue<std::string> temp_queue = error_queue_;
    while (!temp_queue.empty()) {
        errors.push_back(temp_queue.front());
        temp_queue.pop();
    }
    return errors;
}
```

---

### 6.8 printStats() - 打印统计信息

#### 整体级别（重写父类）

```cpp
/**
 * @brief 打印统计信息（重写父类 printStats()）
 * 
 * 沿用父类逻辑：打印 produced_frames_, skipped_frames_, FPS 等
 * 扩展到多个 Group：打印所有 Group 的汇总统计
 */
void printStats() const override {
    LOG_INFO_FMT("MultiWorkerProductionLine Statistics:");
    LOG_INFO_FMT("  Running: %s", isRunning() ? "Yes" : "No");
    LOG_INFO_FMT("  Total Produced: %d frames", getProducedFrames());
    LOG_INFO_FMT("  Total Skipped: %d frames", getSkippedFrames());
    LOG_INFO_FMT("  Average FPS: %.2f", getAverageFPS());
    LOG_INFO_FMT("  Groups: %zu", groups_.size());
}
```

#### Group 级别（新增）

```cpp
void printGroupStats(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return;
    }
    const auto& group = groups_[group_index];
    LOG_INFO_FMT("Group[%zu] '%s' Statistics:", group_index, group->group_id.c_str());
    LOG_INFO_FMT("  Running: %s", isGroupRunning(group_index) ? "Yes" : "No");
    LOG_INFO_FMT("  Producer Frames: %d", getGroupProducerProducedFrames(group_index));
    LOG_INFO_FMT("  Consumer Frames: %d", getGroupConsumerProducedFrames(group_index));
    LOG_INFO_FMT("  Total Frames: %d", getGroupTotalProducedFrames(group_index));
    LOG_INFO_FMT("  Average FPS: %.2f", getGroupTotalAverageFPS(group_index));
}
```

#### 详细统计（已有）

```cpp
void printDetailedStats() const {
    LOG_INFO_FMT("MultiWorkerProductionLine Detailed Statistics:");
    for (size_t i = 0; i < groups_.size(); i++) {
        printGroupStats(i);
    }
}
```

---

## 七、新增查询接口

### 7.1 Group 数量

```cpp
size_t getGroupCount() const { return groups_.size(); }
```

### 7.2 Group 中 Producer/Consumer 数量

```cpp
size_t getGroupProducerCount(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    return groups_[group_index]->producers.size();
}

size_t getGroupConsumerCount(size_t group_index) const {
    if (group_index >= groups_.size() || !groups_[group_index]) {
        return 0;
    }
    return groups_[group_index]->consumers.size();
}
```

### 7.3 统计信息结构体

```cpp
struct Statistics {
    std::atomic<int64_t> total_packets_processed{0};
    std::atomic<int64_t> total_packets_succeeded{0};
    std::atomic<int64_t> total_packets_failed{0};
    std::atomic<int64_t> total_decode_time_us{0};
};

const Statistics& getStatistics() const { return stats_; }
```

---

## 八、BufferPool 统计功能设计（需要添加到 BufferPool）

### 8.1 BufferPool 统计接口（新增）

```cpp
class BufferPool {
public:
    // ====== 统计接口（新增）=====
    
    /**
     * @brief 获取累计生产帧数（submitFilled() 调用次数）
     * 
     * 含义：生产者填充 BufferPool 的总次数
     * 线程安全：是（使用 atomic）
     */
    int64_t getProducedFrames() const { return produced_frames_.load(); }
    
    /**
     * @brief 获取累计消费帧数（releaseFilled() 调用次数）
     * 
     * 含义：消费者使用 BufferPool 的总次数
     * 线程安全：是（使用 atomic）
     */
    int64_t getConsumedFrames() const { return consumed_frames_.load(); }
    
    /**
     * @brief 获取累计跳过帧数（releaseFree() 调用次数）
     * 
     * 含义：生产者填充失败的总次数
     * 线程安全：是（使用 atomic）
     */
    int64_t getSkippedFrames() const { return skipped_frames_.load(); }
    
    /**
     * @brief 获取平均生产 FPS
     * 
     * 计算方式：produced_frames / elapsed_time
     * 线程安全：是
     */
    double getAverageProduceFPS() const;
    
    /**
     * @brief 获取平均消费 FPS
     * 
     * 计算方式：consumed_frames / elapsed_time
     * 线程安全：是
     */
    double getAverageConsumeFPS() const;
    
    /**
     * @brief 重置统计信息
     * 
     * 线程安全：是
     */
    void resetStats();
    
    /**
     * @brief 获取统计信息结构体（用于批量查询）
     */
    struct Statistics {
        int64_t produced_frames;
        int64_t consumed_frames;
        int64_t skipped_frames;
        double average_produce_fps;
        double average_consume_fps;
    };
    Statistics getStatistics() const;

private:
    // ====== 统计成员变量（新增）=====
    
    std::atomic<int64_t> produced_frames_{0};  // 累计生产帧数（submitFilled 调用次数）
    std::atomic<int64_t> consumed_frames_{0}; // 累计消费帧数（releaseFilled 调用次数）
    std::atomic<int64_t> skipped_frames_{0};  // 累计跳过帧数（releaseFree 调用次数）
    std::chrono::steady_clock::time_point stats_start_time_;  // 统计开始时间
};
```

### 8.2 BufferPool 统计实现（需要在 BufferPool.cpp 中实现）

#### 在构造函数中初始化

```cpp
BufferPool::BufferPool(PrivateToken token, const std::string& name, const std::string& category)
    : name_(name)
    , category_(category)
    , registry_id_(0)
    , running_(true)
    , stats_start_time_(std::chrono::steady_clock::now())  // ⭐ 新增：初始化统计开始时间
    , log_prefix_("[BufferPool::" + name + "]")
{
    // ... 现有逻辑 ...
}
```

#### 在 submitFilled() 中更新统计

```cpp
void BufferPool::submitFilled(Buffer* buffer_ptr) {
    // ... 现有逻辑 ...
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // ... 现有逻辑 ...
        
        // 添加到 filled 队列
        filled_queue_.push(buffer_ptr);
        buffer_ptr->setState(Buffer::State::READY_FOR_CONSUME);
        
        // ⭐ 新增：更新生产统计
        produced_frames_.fetch_add(1, std::memory_order_relaxed);
    }
    
    // 通知消费者（锁外通知）
    filled_cv_.notify_one();
}
```

#### 在 releaseFilled() 中更新统计

```cpp
void BufferPool::releaseFilled(Buffer* buffer) {
    // ... 现有逻辑 ...
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // ... 现有逻辑 ...
        
        // 归还到 free 队列
        free_queue_.push(buffer);
        buffer->setState(Buffer::State::IDLE);
        buffer->freeBuffer();
        
        // ⭐ 新增：更新消费统计
        consumed_frames_.fetch_add(1, std::memory_order_relaxed);
    }
    
    // 通知生产者（锁外通知）
    free_cv_.notify_one();
}
```

#### 在 releaseFree() 中更新统计

```cpp
void BufferPool::releaseFree(Buffer* buffer_ptr) {
    // ... 现有逻辑 ...
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // ... 现有逻辑 ...
        
        // 归还到 free 队列
        free_queue_.push(buffer_ptr);
        buffer_ptr->setState(Buffer::State::IDLE);
        
        // ⭐ 新增：更新跳过统计
        skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    }
    
    // 通知生产者（锁外通知）
    free_cv_.notify_one();
}
```

#### 实现 FPS 计算

```cpp
double BufferPool::getAverageProduceFPS() const {
    auto duration = std::chrono::steady_clock::now() - stats_start_time_;
    double seconds = std::chrono::duration<double>(duration).count();
    if (seconds > 0) {
        return produced_frames_.load() / seconds;
    }
    return 0.0;
}

double BufferPool::getAverageConsumeFPS() const {
    auto duration = std::chrono::steady_clock::now() - stats_start_time_;
    double seconds = std::chrono::duration<double>(duration).count();
    if (seconds > 0) {
        return consumed_frames_.load() / seconds;
    }
    return 0.0;
}
```

---

## 九、设计原则总结

### 9.1 沿用父类设计

- 使用父类的 `running_`, `start_time_`, `error_callback_` 等成员变量
- 重写父类方法，保持向后兼容
- 使用父类的 `VideoProductionLine` 作为生产者

### 9.2 简化结构

- 去掉 Runtime 包装，直接存储对象
- 使用简单的 `GroupData` 结构
- 连接器直接存储，不需要 ConnectorRuntime

### 9.3 三级接口设计

- 整体级别：重写父类方法，向后兼容
- Group 级别：按 Group 查询统计
- Producer/Consumer 级别：精确查询

### 9.4 基于 BufferPool 统计

- Consumer 统计通过 BufferPool 的统计接口获取
- 不需要在 MultiWorker 中维护 Consumer 统计
- 统计更准确，来自数据源

---

## 十、start() 和 groupThreadFunc() 函数设计

### 10.1 validateConfig() - 配置校验函数（私有）

**设计目的：**
- 在 start() 函数中调用，确保配置有效
- 提前发现配置错误，避免运行时错误

**校验内容：**

1. **每个 Group 必须至少有一个生产者和一个消费者**
   - 检查 `group.producer_configs.empty()`
   - 检查 `group.consumer_configs.empty()`

2. **每个 Group 必须至少有一个连接器**
   - 检查 `group.connector_configs.empty()`

3. **连接器的 producer_ids 和 consumer_ids 必须存在**
   - 遍历所有连接器的 `producer_ids`，检查是否在 `group.producer_configs` 中存在
   - 遍历所有连接器的 `consumer_ids`，检查是否在 `group.consumer_configs` 中存在

4. **连接器模式校验**
   - `ONE_TO_ONE`：生产者数量必须等于消费者数量
   - `ONE_TO_MANY`：必须只有1个生产者
   - `MANY_TO_ONE`：必须只有1个消费者
   - `MANY_TO_MANY`：至少1个生产者，至少1个消费者（已在上面检查）

5. **检查是否有未连接的 Producer/Consumer**
   - 收集所有连接器关联的 `producer_ids`，检查是否有 Producer 未被连接
   - 收集所有连接器关联的 `consumer_ids`，检查是否有 Consumer 未被连接

**返回值：**
- `true`：配置有效
- `false`：配置无效（会调用 `setError()` 设置错误信息）

---

### 10.2 start() - 启动函数

**设计要点：**
- 使用全局线程池单例（GlobalThreadPool）
- 调用 `validateConfig()` 校验配置
- 创建所有 Producer、Consumer、Connector
- 建立绑定关系
- 启动所有 Group 线程

**详细流程：**

1. **检查运行状态**
   - 如果已在运行，返回 false

2. **初始化全局线程池**
   - 调用 `GlobalThreadPool::getInstance().setSize(config_.thread_pool_size)`

3. **校验配置**
   - 调用 `validateConfig()`
   - 如果校验失败，返回 false

4. **为每个 Group 创建运行时环境**
   - 创建 `GroupData` 对象
   - 创建所有生产者（调用父类 `VideoProductionLine::start()`）
   - 获取生产者的 BufferPool 信息并保存
   - 创建所有消费者（不 open，等待连接器绑定）
   - 创建所有连接器并建立绑定关系：
     - 通过 `producer_ids` 和 `consumer_ids` 找到索引
     - 创建 `Connector` 对象
     - 为每个消费者设置源 BufferPool（`consumer->setSourceBufferPool()`）
     - 打开消费者（`consumer->open()`）
     - 保存消费者的输出 BufferPool 信息

5. **初始化状态**
   - 设置 `running_ = true`
   - 设置 `start_time_`

6. **启动所有 Group 线程**
   - 为每个 Group 创建线程，执行 `groupThreadFunc()`
   - 如果线程启动失败，清理并返回 false

**返回值：**
- `true`：启动成功
- `false`：启动失败（会调用 `setError()` 设置错误信息）

---

### 10.3 groupThreadFunc() - Group 线程函数

**设计要点：**
- 使用全局线程池提交 Consumer 任务
- 使用 CountDownLatch 同步等待
- 统计和错误处理
- 循环执行

**详细流程：**

1. **初始化**
   - 获取全局线程池引用
   - 记录日志

2. **主循环（`while (group->is_running.load() && running_.load())`）**
   
   a. **统计活跃的消费者**
      - 遍历所有连接器，统计已打开且活跃的消费者数量
      - 如果没有活跃消费者，等待 100ms 后继续
   
   b. **创建同步门闩**
      - 创建 `CountDownLatch(active_consumers)`
      - 创建 `success_count` 和 `error_count` 原子变量
   
   c. **提交任务给所有活跃消费者**
      - 遍历所有连接器
      - 对于每个活跃消费者，提交任务到全局线程池：
        - 调用 `consumer->fillBuffer(0, dummy_buffer)`（Buffer 模式下 frame_index 被忽略）
        - 根据结果更新 `success_count` 或 `error_count`
        - 调用 `latch->countDown()`
   
   d. **同步等待所有消费者完成**
      - 调用 `latch->wait(DEFAULT_TIMEOUT_MS)`（默认 5 秒超时）
      - 如果超时，记录错误日志
      - 如果成功，更新统计信息：
        - `group->processed_count++`
        - `group->success_count += success_count`
        - `group->error_count += error_count`
        - `stats_.total_packets_processed++`
        - `stats_.total_packets_succeeded += success_count`
        - `stats_.total_packets_failed += error_count`
   
   e. **短暂延迟**
      - `std::this_thread::sleep_for(std::chrono::milliseconds(1))`

3. **线程退出**
   - 记录日志（包含统计信息）

**注意事项：**
- 使用全局线程池，而不是每个 Group 创建自己的线程池
- 使用 CountDownLatch 同步等待，确保所有消费者完成后再继续
- 超时时间使用固定值（5秒），不依赖配置
- 统计信息在 Group 级别和全局级别都更新

---

## 十一、全局线程池设计

### 11.1 GlobalThreadPool - 全局线程池单例

**设计目的：**
- 整个项目只有一个线程池实例，避免资源浪费
- 所有 MultiWorkerProductionLine 共享同一个线程池
- 线程安全：使用单例模式 + 互斥锁保护

**实现位置：**
- 头文件：`packages/components/include/common/GlobalThreadPool.hpp`
- 实现文件：`packages/components/source/common/GlobalThreadPool.cpp`

**核心接口：**
- `getInstance()`：获取全局单例
- `getThreadPool()`：获取线程池引用（如果未初始化会自动使用默认大小初始化）
- `setSize(int size)`：设置线程池大小（只在第一次调用时生效）
- `wait()`：等待所有任务完成

**使用方式：**
```cpp
// 初始化（可选，如果未初始化会自动使用默认大小）
GlobalThreadPool::getInstance().setSize(8);

// 获取线程池引用
auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
thread_pool.detach_task([...]() { ... });
```

---

## 十二、实现步骤

1. ✅ 设计 Connector 类
2. ✅ 设计配置结构
3. ✅ 设计运行时结构
4. ✅ 设计查询接口（三级设计）
5. ✅ 设计 BufferPool 统计功能
6. ✅ 设计全局线程池（GlobalThreadPool）
7. ✅ 设计 validateConfig() 函数
8. ✅ 设计 start() 函数
9. ✅ 设计 groupThreadFunc() 函数
10. ⏳ 实现 Connector 类
11. ⏳ 实现所有查询接口
12. ⏳ 在 BufferPool 中添加统计功能
13. ⏳ 实现 validateConfig() 函数
14. ⏳ 实现 start() 函数
15. ⏳ 实现 groupThreadFunc() 函数
16. ⏳ 测试和验证

---

**文档版本：** v2.0  
**最后更新：** 2025-01-27  
**状态：** 设计方案已全部确认，可以开始实现
