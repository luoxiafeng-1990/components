# BufferConsumer 组件设计说明

## 设计思路

BufferConsumer 组件采用**策略模式 + 服务门面模式**的混合架构，将消费逻辑与基础设施管理分离，实现高度的可扩展性和易用性。

### 核心设计理念

**分离关注点**：将"消费策略"（如何消费）与"服务管理"（何时消费、如何管理资源）完全解耦。

- **策略层**（`IBufferConsumer`）：定义消费算法的抽象接口，具体实现包括显示、文件写入、多通道写入等策略，遵循单一职责原则，只负责消费逻辑。
- **服务层**（`BufferConsumerService`）：作为上下文和服务门面，管理视频生产线、缓冲池、PSNR对比、统计信息等基础设施，不感知具体策略实现。
- **配置层**：采用分层配置结构，将服务配置（`Config`：生产线、运行时、PSNR参数）与消费者配置（`ConsumerConfig`：策略类型和策略参数）分离，通过 Builder 模式简化配置构建。

### 使用方式

**三步完成**：配置服务 → 配置消费者 → 一键执行

```cpp
// 1. 配置服务（管理基础设施）
auto service_config = ConsumerConfigBuilder()
    .setWorkerConfig(workerConfig)      // 解码器配置
    .setMaxFrames(1000)                 // 运行时参数
    .setEnablePSNRCompare(true)         // PSNR对比
    .build();

// 2. 配置消费者（选择消费策略）
auto consumer_config = ConsumerStrategyConfigBuilder()
    .setType(ConsumerConfig::Type::DISPLAY)  // 选择显示策略
    .setDisplayDevice(&display)
    .setDisplayChannels(true, false)
    .build();

// 3. 一键执行（自动完成：启动生产线 → 消费Buffer → 统计 → 清理）
BufferConsumerService service;
std::atomic<bool> running(true);
ErrorCallback error_handler = [](const ConsumerErrorInfo& error) {
    LOG_ERROR("Error: " << error.toString());
};
service.execute(service_config, consumer_config, &running, error_handler);
```

### 设计优势

1. **开闭原则**：新增消费策略只需实现 `IBufferConsumer` 接口，无需修改服务代码。
2. **配置灵活**：服务配置与消费者配置分离，可独立调整基础设施参数和策略参数。
3. **错误处理增强**：提供 `ConsumerErrorInfo` 结构，包含错误码、位置、上下文等完整信息，便于诊断。
4. **生命周期自动化**：`execute()` 方法封装完整的生命周期管理，减少样板代码。
5. **多通道支持**：内置支持双通道（PP0/PP1）的独立消费和PSNR对比。

### 架构层次

```
┌─────────────────────────────────────────┐
│  应用层：使用 BufferConsumerService      │
│  (execute() 一键执行)                    │
└─────────────────────────────────────────┘
           ↓
┌─────────────────────────────────────────┐
│  服务层：BufferConsumerService          │
│  (管理生产线、缓冲池、PSNR、统计)        │
└─────────────────────────────────────────┘
           ↓
┌─────────────────────────────────────────┐
│  策略层：IBufferConsumer 接口           │
│  (DisplayConsumer, FileWriterConsumer   │
│   MultiChannelFileWriterConsumer...)     │
└─────────────────────────────────────────┘
           ↓
┌─────────────────────────────────────────┐
│  基础设施：VideoProductionLine          │
│  BufferPool, BufferComparator...        │
└─────────────────────────────────────────┘
```

这种设计使得组件既易于使用（一键执行），又易于扩展（新增策略），同时保持了清晰的职责分离和良好的可测试性。
