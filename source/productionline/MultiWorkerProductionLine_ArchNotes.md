## MultiWorkerProductionLine 架构变更摘要（Connector + 全局线程池版）

> 说明：本文件是对 `ARCHITECTURE.md` 中 MultiWorkerProductionLine 相关章节的更新补充，描述当前最新实现（基于 Connector 的多生产者/多消费者架构和全局线程池）。历史版本设计仍可在 `ARCHITECTURE.md` 中参考。

### 1. 设计目标回顾

- 支持 **每个 Group 内多个生产者 + 多个消费者**。
- 引入 **Connector 连接器**，用来显式描述 Producer ↔ Consumer 的拓扑关系（1:1、1:N、N:1、N:M）。
- 使用 **全局线程池 GlobalThreadPool**（内部封装 `BS::thread_pool`），避免每个流水线各自创建线程池。
- 保持与 `VideoProductionLine` 的设计一致：Producer 仍由 `VideoProductionLine` 驱动，从数据源填充 BufferPool。

### 2. 配置结构（最新）

```cpp
// 生产者配置
struct ProducerConfig {
    std::string producer_id;      // 组内唯一标识
    WorkerConfig worker_config;   // 复用现有 WorkerConfig
};

// 消费者配置
struct ConsumerConfig {
    std::string consumer_id;      // 组内唯一标识
    WorkerConfig worker_config;   // 复用现有 WorkerConfig
};

// 连接器配置：描述 Producer ↔ Consumer 的映射关系
struct ConnectorConfig {
    Connector::Mode mode;                 // ONE_TO_ONE / ONE_TO_MANY / MANY_TO_ONE / MANY_TO_MANY
    std::vector<std::string> producer_ids;  // 此连接器关联的生产者 ID 列表
    std::vector<std::string> consumer_ids;  // 此连接器关联的消费者 ID 列表
};

// WorkerGroup：一个 Group = 多个 Producer + 多个 Consumer + 多个 Connector
struct WorkerGroup {
    std::string group_id;
    std::vector<ProducerConfig> producer_configs;
    std::vector<ConsumerConfig> consumer_configs;
    std::vector<ConnectorConfig> connector_configs;
};

// MultiWorker 总配置
struct MultiWorkerConfig {
    std::vector<WorkerGroup> groups;
    int thread_pool_size = 4;   // 用于初始化全局线程池 GlobalThreadPool
};
```

### 3. Connector 连接器

```cpp
class Connector {
public:
    enum class Mode {
        ONE_TO_ONE,      // 1:1
        ONE_TO_MANY,     // 1:N（一个 Producer → 多个 Consumer）
        MANY_TO_ONE,     // N:1（多个 Producer → 一个 Consumer，简化为绑定第一个 Producer）
        MANY_TO_MANY     // N:M（轮询策略）
    };

    Connector(Mode mode,
              const std::vector<size_t>& producer_indices,
              const std::vector<size_t>& consumer_indices);

    // 返回：给定 consumer_index（在 connector 自己的 consumer 集合中的索引），对应的 producer 在 Group 中的索引
    int getProducerIndexForConsumer(size_t consumer_index) const;
};
```

- `producer_indices` / `consumer_indices`：指向 `GroupData::producers` / `GroupData::consumers` 的索引。
- `computeMapping()` 根据 `Mode` 生成 `consumer_index -> producer_index` 的映射表。
- Connector 只负责**路由关系**，不直接处理数据。

### 4. 运行时结构 GroupData

```cpp
struct GroupData {
    std::string group_id;

    struct ProducerInfo {
        std::string producer_id;
        std::unique_ptr<VideoProductionLine> producer_line; // 复用父类生产逻辑
        uint64_t buffer_pool_id{0};
        std::weak_ptr<BufferPool> buffer_pool_weak;
    };
    std::vector<std::unique_ptr<ProducerInfo>> producers;
    std::unordered_map<std::string, ProducerInfo*> producer_by_id;

    struct ConsumerInfo {
        std::string consumer_id;
        std::shared_ptr<BufferFillingWorkerFacade> worker;
        uint64_t buffer_pool_id{0};               // Consumer 输出 BufferPool（给下游用）
        std::weak_ptr<BufferPool> buffer_pool_weak;
    };
    std::vector<std::unique_ptr<ConsumerInfo>> consumers;
    std::unordered_map<std::string, ConsumerInfo*> consumer_by_id;

    std::vector<std::unique_ptr<Connector>> connectors;

    std::thread group_thread;
    std::atomic<bool> is_running{false};

    std::atomic<int64_t> processed_count{0};
    std::atomic<int64_t> success_count{0};
    std::atomic<int64_t> error_count{0};
};
```

### 5. 全局线程池 GlobalThreadPool

- 位置：`include/common/GlobalThreadPool.hpp`
- 所有 `MultiWorkerProductionLine` 实例共享同一个 `BS::thread_pool` 实例：

```cpp
class GlobalThreadPool {
public:
    static GlobalThreadPool& getInstance();

    // 如未初始化，则使用 default_size_ 创建
    BS::thread_pool<>& getThreadPool();

    // 仅首次调用前有效；后续调用不会重新创建线程池
    void setSize(int size);

    void wait(); // 等待所有任务完成
};
```

在 `MultiWorkerProductionLine::start()` 中：

```cpp
GlobalThreadPool::getInstance().setSize(config_.thread_pool_size);
auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
```

在 `groupThreadFunc()` 中：

```cpp
auto& thread_pool = GlobalThreadPool::getInstance().getThreadPool();
thread_pool.detach_task([...]{
    consumer_info->worker->fillBuffer(0, nullptr);
    // ...
});
```

### 6. start() 流程（简要）

1. `validateConfig()` 校验：
   - 每个 Group 至少 1 个 Producer、1 个 Consumer、1 个 Connector。
   - Connector 引用的 `producer_ids` / `consumer_ids` 必须存在。
   - 校验 `Mode` 与数量关系（1:1 / 1:N / N:1 / N:M）。
   - 检查是否有未被任何 Connector 连接的 Producer / Consumer。

2. 为每个 Group：
   - 创建所有 `VideoProductionLine` Producer，并 `start()`，记录其工作 BufferPool。
   - 创建所有 Consumer（`BufferFillingWorkerFacade`），配置为 buffer 模式，但暂不 open。
   - 创建所有 Connector，建立 Producer/Consumer 索引映射。
   - 根据 Connector 映射：
     - 为 Consumer 设置源 BufferPool（绑定对应 Producer 的 BufferPool）。
     - `open()` Consumer，并记录其输出 BufferPool ID。

3. 设置 `running_ = true`，记录 `start_time_`。
4. 启动每个 Group 独立线程执行 `groupThreadFunc(GroupData*)`。

### 7. groupThreadFunc() 流程（简要）

1. 通过所有 Connector 统计当前活跃的 Consumer 数量。
2. 创建 `CountDownLatch(active_consumers)`，并准备 `success_count` / `error_count`。
3. 对每个活跃 Consumer：
   - 提交到全局线程池：调用 `fillBuffer(0, nullptr)`（buffer 模式从绑定的 Producer BufferPool 拉流）。
   - 根据结果更新统计，并 `latch->countDown()`。
4. `latch->wait(DEFAULT_TIMEOUT_MS)`（固定 5 秒）等待所有任务完成：
   - 成功：`processed_count++`、更新成功/失败计数和全局统计。
   - 超时：记录错误日志。
5. 短暂 `sleep_for(1ms)`，避免 busy loop。

### 8. 与旧设计的差异

- **Producer 数量**：由「每组 1 个 Producer」扩展为「每组多个 Producer」。
- **拓扑描述**：新增 Connector 层，显式配置 Producer ↔ Consumer 映射（支持多种模式），取代「1 Producer → N Consumer」的固定关系。
- **线程池**：从「MultiWorker 内部持有 `std::unique_ptr<BS::thread_pool>`」升级为「全局单例 GlobalThreadPool」，所有流水线/Group 共享同一个线程池实例。
- **运行时结构**：由 `WorkerGroupRuntime` 简化为 `GroupData + ProducerInfo + ConsumerInfo + Connector`，结构更清晰，也更贴近当前实现。

