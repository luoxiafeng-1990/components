# WorkerSyncCoordinator 使用示例 (v2.23)

## 概述

`WorkerSyncCoordinator` 是一个用于协调同一 Connector 内多个 Worker 的同步机制，允许在 Worker 解码完成后、提交 Buffer 前执行用户自定义的回调函数（如 PSNR 对比、质量检测等）。

---

## 架构说明

```
MultiWorkerProductionLine
    ↓
WorkerGroup
    ↓
Connector (ONE_TO_MANY)
    ↓
Producer: hw_recorder
    ↓
Consumers: hw_decoder, sw_decoder
    ↓
WorkerSyncCoordinator ⭐
    ↓
Callback Chain: [PSNR对比, 质量检测]
```

---

## 使用示例

### 示例 1：PSNR 对比

```cpp
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/WorkerSyncCoordinator.hpp"
#include <cmath>

// ========== 步骤1：定义上下文 ==========
struct PSNRContext {
    std::string worker1_name = "hw_decoder";
    std::string worker2_name = "sw_decoder";
    double psnr_threshold = 30.0;  // dB
    std::atomic<int> mismatch_count{0};
    std::atomic<int> total_frames{0};
    log4cplus::Logger logger;
};

// ========== 步骤2：实现 PSNR 计算 ==========
double calculatePSNR(Buffer* buf1, Buffer* buf2) {
    const uint8_t* data1 = buf1->getData();
    const uint8_t* data2 = buf2->getData();
    size_t size = buf1->getDataSize();
    
    if (size != buf2->getDataSize()) {
        throw std::runtime_error("Buffer size mismatch");
    }
    
    // 计算 MSE (Mean Squared Error)
    double mse = 0.0;
    for (size_t i = 0; i < size; i++) {
        double diff = static_cast<double>(data1[i]) - static_cast<double>(data2[i]);
        mse += diff * diff;
    }
    mse /= size;
    
    // 计算 PSNR
    if (mse == 0.0) {
        return 100.0;  // 完全相同
    }
    
    double max_pixel = 255.0;
    double psnr = 10.0 * log10((max_pixel * max_pixel) / mse);
    
    return psnr;
}

// ========== 步骤3：实现回调函数 ==========
bool psnrCompareCallback(
    uint64_t frame_version,
    const std::map<std::string, Buffer*>& worker_buffers,
    void* ctx
) {
    auto* context = static_cast<PSNRContext*>(ctx);
    context->total_frames++;
    
    // 获取两个 Worker 的 Buffer
    auto it1 = worker_buffers.find(context->worker1_name);
    auto it2 = worker_buffers.find(context->worker2_name);
    
    if (it1 == worker_buffers.end() || it2 == worker_buffers.end()) {
        LOG4CPLUS_ERROR(context->logger, "Worker Buffer 不完整");
        return false;
    }
    
    Buffer* buf1 = it1->second;
    Buffer* buf2 = it2->second;
    
    try {
        // 计算 PSNR
        double psnr = calculatePSNR(buf1, buf2);
        
        LOG4CPLUS_INFO_FMT(context->logger, 
            "[Frame %llu] PSNR = %.2f dB (%s vs %s)", 
            (unsigned long long)frame_version, psnr,
            context->worker1_name.c_str(),
            context->worker2_name.c_str());
        
        // 检查阈值
        if (psnr < context->psnr_threshold) {
            context->mismatch_count++;
            LOG4CPLUS_WARN_FMT(context->logger, 
                "[Frame %llu] ⚠️ PSNR 低于阈值 (%.2f < %.2f)", 
                (unsigned long long)frame_version, psnr, context->psnr_threshold);
        }
        
        return true;  // 继续提交
        
    } catch (const std::exception& e) {
        LOG4CPLUS_ERROR_FMT(context->logger, 
            "[Frame %llu] PSNR 计算失败: %s", 
            (unsigned long long)frame_version, e.what());
        return false;  // 跳过提交
    }
}

// ========== 步骤4：配置 MultiWorkerProductionLine ==========
int main() {
    // 创建 PSNR 上下文
    PSNRContext psnr_ctx;
    psnr_ctx.logger = log4cplus::Logger::getInstance("PSNR");
    
    // 配置 WorkerGroup
    WorkerGroupConfig group_config;
    group_config.group_id = "decoder_group";
    
    // 配置生产者（Record Worker）
    ProducerConfig producer_cfg;
    producer_cfg.producer_name = "hw_recorder";
    producer_cfg.worker_config = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath("rtsp://example.com/stream")
                .setBufferCount(64)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
        .build();
    group_config.producer_configs.push_back(producer_cfg);
    
    // 配置消费者1（硬件解码器）
    ConsumerConfig consumer1_cfg;
    consumer1_cfg.consumer_name = "hw_decoder";
    consumer1_cfg.worker_config = WorkerConfigBuilder()
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", TacoConfigBuilder().setChannels(true, false).build())
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_DECODE)
        .build();
    group_config.consumer_configs.push_back(consumer1_cfg);
    
    // 配置消费者2（软件解码器）
    ConsumerConfig consumer2_cfg;
    consumer2_cfg.consumer_name = "sw_decoder";
    consumer2_cfg.worker_config = WorkerConfigBuilder()
        .setDecoderConfig(DecoderConfigBuilder().useSoftware().build())
        .setWorkerType(WorkerType::FFMPEG_DECODE)
        .build();
    group_config.consumer_configs.push_back(consumer2_cfg);
    
    // ⭐ 配置 Connector（启用帧同步）
    ConnectorConfig conn_cfg;
    conn_cfg.mode = Connector::Mode::ONE_TO_MANY;
    conn_cfg.producer_names = {"hw_recorder"};
    conn_cfg.consumer_names = {"hw_decoder", "sw_decoder"};
    
    // ⭐ 启用帧同步
    conn_cfg.enable_frame_sync = true;
    
    // ⭐ 添加回调：PSNR 对比
    conn_cfg.callback_chain.push_back(CallbackChainItem{
        psnrCompareCallback,
        &psnr_ctx,
        "PSNR对比"
    });
    
    group_config.connector_configs.push_back(conn_cfg);
    
    // 创建 MultiWorkerConfig
    MultiWorkerConfig config;
    config.groups.push_back(group_config);
    config.thread_pool_size = 4;
    
    // 创建并启动 MultiWorkerProductionLine
    MultiWorkerProductionLine line(config);
    
    if (!line.start()) {
        std::cerr << "Failed to start production line" << std::endl;
        return 1;
    }
    
    std::cout << "Production line started, press Enter to stop..." << std::endl;
    std::cin.get();
    
    line.stop();
    
    // 输出统计
    LOG4CPLUS_INFO_FMT(psnr_ctx.logger, 
        "========== PSNR 统计 ==========");
    LOG4CPLUS_INFO_FMT(psnr_ctx.logger, 
        "总帧数: %d", psnr_ctx.total_frames.load());
    LOG4CPLUS_INFO_FMT(psnr_ctx.logger, 
        "不匹配帧数: %d", psnr_ctx.mismatch_count.load());
    LOG4CPLUS_INFO_FMT(psnr_ctx.logger, 
        "匹配率: %.2f%%", 
        100.0 * (psnr_ctx.total_frames.load() - psnr_ctx.mismatch_count.load()) 
        / psnr_ctx.total_frames.load());
    
    return 0;
}
```

---

### 示例 2：多个回调（回调链）

```cpp
// ========== 回调1：PSNR 对比 ==========
bool psnrCallback(uint64_t frame_version, 
                  const std::map<std::string, Buffer*>& buffers,
                  void* ctx) {
    // ... PSNR 计算 ...
    return true;  // 继续执行下一个回调
}

// ========== 回调2：质量检测 ==========
struct QualityContext {
    int min_brightness = 10;
    int max_brightness = 245;
};

bool qualityCallback(uint64_t frame_version,
                     const std::map<std::string, Buffer*>& buffers,
                     void* ctx) {
    auto* context = static_cast<QualityContext*>(ctx);
    
    // 检查所有 Worker 的 Buffer 质量
    for (const auto& [name, buffer] : buffers) {
        int brightness = calculateBrightness(buffer);
        
        if (brightness < context->min_brightness || 
            brightness > context->max_brightness) {
            LOG_WARN("Worker '" << name << "' 亮度异常: " << brightness);
            return false;  // 质量不合格，拒绝提交
        }
    }
    
    return true;  // 质量合格，继续
}

// ========== 回调3：日志记录 ==========
bool logCallback(uint64_t frame_version,
                 const std::map<std::string, Buffer*>& buffers,
                 void* ctx) {
    LOG_INFO("Frame " << frame_version << " 处理完成，Worker 数量: " << buffers.size());
    return true;
}

// ========== 配置回调链 ==========
PSNRContext psnr_ctx;
QualityContext quality_ctx;

ConnectorConfig conn_cfg;
conn_cfg.enable_frame_sync = true;

// 添加多个回调（按顺序执行）
conn_cfg.callback_chain.push_back(CallbackChainItem{
    psnrCallback, &psnr_ctx, "PSNR对比"
});
conn_cfg.callback_chain.push_back(CallbackChainItem{
    qualityCallback, &quality_ctx, "质量检测"
});
conn_cfg.callback_chain.push_back(CallbackChainItem{
    logCallback, nullptr, "日志记录"
});
```

**执行流程**：
1. 所有 Worker 到达同步点
2. 执行回调1（PSNR对比）→ 返回 true
3. 执行回调2（质量检测）→ 返回 true
4. 执行回调3（日志记录）→ 返回 true
5. 所有回调通过，允许提交

**如果任何回调返回 false**：
- 终止回调链
- 拒绝提交所有 Worker 的 Buffer

---

### 示例 3：采样对比（性能优化）

```cpp
// 每 10 帧对比一次，减少性能影响
bool psnrCallbackSampled(
    uint64_t frame_version,
    const std::map<std::string, Buffer*>& buffers,
    void* ctx
) {
    // 每 10 帧对比一次
    if (frame_version % 10 != 0) {
        return true;  // 跳过对比，直接通过
    }
    
    // 执行 PSNR 对比
    return psnrCompareCallback(frame_version, buffers, ctx);
}
```

---

## 配置选项

### ConnectorConfig 新增字段

```cpp
struct ConnectorConfig {
    Connector::Mode mode;
    std::vector<std::string> producer_names;
    std::vector<std::string> consumer_names;
    
    // ⭐ v2.23 新增
    bool enable_frame_sync = false;          // 是否启用帧同步
    CallbackChain callback_chain;            // 回调链
};
```

### 回调函数签名

```cpp
using FrameSyncCallback = std::function<bool(
    uint64_t frame_version,           // 帧版本号
    const std::map<std::string, Buffer*>& worker_buffers,  // Worker -> Buffer
    void* context                     // 用户上下文
)>;
```

**返回值**：
- `true`: 继续执行下一个回调（或允许提交）
- `false`: 终止回调链并拒绝提交

---

## 性能考虑

### 1. 同步开销

帧同步会导致快的 Worker 等待慢的 Worker，降低吞吐量。

**建议**：
- 仅在调试/验证阶段启用
- 生产环境关闭或使用采样对比

### 2. 采样策略

```cpp
// 每 N 帧对比一次
if (frame_version % N != 0) {
    return true;  // 跳过
}
```

### 3. 快速路径

如果不配置回调，`WorkerSyncCoordinator::arrive()` 直接返回 `true`，零开销。

```cpp
bool WorkerSyncCoordinator::arrive(...) {
    // 快速路径：无回调
    if (callback_chain_.empty()) {
        return true;  // 直接返回，无同步开销
    }
    
    // 慢路径：执行同步和回调链
    // ...
}
```

---

## 注意事项

1. **回调执行线程**：回调在最后一个到达的 Worker 线程中执行，其他 Worker 线程阻塞等待
2. **回调应快速执行**：避免阻塞过久影响性能
3. **异常处理**：回调抛异常视为失败，返回 `false`
4. **Buffer 生命周期**：回调中不要修改 Buffer 的所有权，只读取数据
5. **线程安全**：回调中访问共享数据需要加锁

---

## 常见问题

### Q1: 如何禁用帧同步？

```cpp
conn_cfg.enable_frame_sync = false;  // 或不配置回调
conn_cfg.callback_chain = {};
```

### Q2: 如何为不同 Connector 配置不同回调？

每个 `ConnectorConfig` 独立配置：

```cpp
// Connector 1: PSNR 对比
conn1_cfg.callback_chain = {
    {psnrCallback, &ctx1, "PSNR"}
};

// Connector 2: 质量检测
conn2_cfg.callback_chain = {
    {qualityCallback, &ctx2, "Quality"}
};
```

### Q3: 回调返回 false 后会发生什么？

- 所有 Worker 的 Buffer 被释放（不提交）
- 统计计数器不增加
- Worker 继续处理下一帧

---

## 版本历史

- **v2.23**: 引入 `WorkerSyncCoordinator` 和回调链机制
- **v2.22**: 修复 `BufferPacketSource` 资源泄漏和死锁
- **v2.18**: 引入共享模式（ONE_TO_MANY）

---

## 参考

- `WorkerSyncCoordinator.hpp`: 协调器接口
- `MultiWorkerProductionLine.hpp`: 集成点
- `WorkerConfig.hpp`: 配置结构
- `BUGFIX_v2.22.md`: v2.22 修复报告
