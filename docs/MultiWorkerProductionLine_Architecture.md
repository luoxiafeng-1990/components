# MultiWorkerProductionLine + WorkerSyncCoordinator 技术架构文档

## 1. 概述

`MultiWorkerProductionLine` 是一个多 Worker 并行执行引擎，支持一个或多个生产者与多个消费者的协调工作。`WorkerSyncCoordinator` 是其核心同步组件，负责在多 Worker 完成解码后执行帧级同步比较。

```
┌─────────────────────────────────────────────────────────────┐
│               MultiWorkerProductionLine                      │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                  WorkerGroupRuntime                    │   │
│  │                                                       │   │
│  │  ┌─────────────┐     ┌──────────────────────────┐    │   │
│  │  │ ProducerInfo│     │      Connector            │    │   │
│  │  │ (RecordWorker│     │  ┌──────────────────┐    │    │   │
│  │  │  + BufferPool│────▶│  │ SharedSource      │    │    │   │
│  │  │  + ProdLine) │     │  │ (EncodedPacket    │    │    │   │
│  │  └─────────────┘     │  │  SourceFromBuffer) │    │    │   │
│  │                       │  └────────┬───────────┘    │    │   │
│  │                       │           │                │    │   │
│  │                       │     ┌─────┴─────┐         │    │   │
│  │                       │     ▼           ▼         │    │   │
│  │  ┌─────────────┐     │  ┌──────┐  ┌──────┐      │    │   │
│  │  │ConsumerInfo │     │  │Worker│  │Worker│      │    │   │
│  │  │ (HW Decoder)│◀────│  │ (HW) │  │ (SW) │      │    │   │
│  │  └─────────────┘     │  └───┬──┘  └───┬──┘      │    │   │
│  │  ┌─────────────┐     │      │          │         │    │   │
│  │  │ConsumerInfo │     │      ▼          ▼         │    │   │
│  │  │ (SW Decoder)│◀────│  ┌──────────────────┐    │    │   │
│  │  └─────────────┘     │  │WorkerSyncCoordinator│  │    │   │
│  │                       │  │  arrive() → compare │  │    │   │
│  │                       │  └──────────────────┘    │    │   │
│  │                       └──────────────────────────┘    │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 2. MultiWorkerProductionLine 核心函数

### 2.1 构造函数

```cpp
// 文件: components/include/productionline/line/MultiWorkerProductionLine.hpp:126
MultiWorkerProductionLine(
    const MultiWorkerConfig& config,
    bool loop = false,
    int thread_count = 1,
    bool enable_monitor = false
);
```

- 继承自 `VideoProductionLine`
- `config_` 存储 `MultiWorkerConfig`（包含多个 `WorkerGroupConfig`）
- 初始化 Logger 和日志前缀

### 2.2 start() - 启动流水线

```cpp
// 文件: components/source/productionline/line/MultiWorkerProductionLine.cpp
bool MultiWorkerProductionLine::start();
```

启动流程：
1. `validateConfig()` - 校验所有 Group 配置（生产者/消费者/连接器完整性）
2. 遍历 `config_.groups`，为每个 Group：
   - `createProducersForGroup()` - 创建生产者 Worker 和 VideoProductionLine
   - `createConnectorsForGroup()` - 创建 Connector 和 `EncodedPacketSourceFromBuffer`
   - `createConsumersForGroup()` - 创建消费者 Worker（解码器）
3. `startGroupThreads()` - 为每个 Group 启动调度线程
4. `startStatsReportTimer()` - 启动统计报告定时器

### 2.3 groupThreadFunc() - Group 调度线程

```cpp
void groupThreadFunc(const std::shared_ptr<WorkerGroupRuntime>& group);
```

核心逻辑：
1. 启动生产者 `producer_line->start()`
2. 为每个 Consumer 提交 `workerThreadFunc` 到全局线程池
3. 使用 `CountDownLatch` 等待所有 Worker 线程结束

### 2.4 workerThreadFunc() - Worker 常驻线程（核心循环）

```cpp
void workerThreadFunc(
    const std::shared_ptr<WorkerGroupRuntime>& group, 
    WorkerGroupRuntime::ConsumerInfo* consumer_info,
    const std::string& consumer_name
);
```

**核心循环流程：**

```
┌─────────────────────────────────────────────────┐
│            Worker Thread Main Loop               │
│                                                  │
│  ┌──────────────────────────┐                    │
│  │ 1. acquireFreeBuffer()   │  从 BufferPool     │
│  │    (blocking wait)       │  获取空闲 Buffer   │
│  └───────────┬──────────────┘                    │
│              ▼                                    │
│  ┌──────────────────────────┐                    │
│  │ 2. fillBuffer()          │  调用 Worker 的    │
│  │    → readAndSendPacket() │  解码/编码函数     │
│  └───────────┬──────────────┘                    │
│              ▼                                    │
│  ┌──────────────────────────┐                    │
│  │ 3. performFrameSync()    │  帧同步（如启用）  │
│  │    → arrive()            │  阻塞等待其他Worker│
│  └───────────┬──────────────┘                    │
│              ▼                                    │
│  ┌──────────────────────────┐                    │
│  │ 4. submitFilledBuffer()  │  提交到 filled     │
│  │    or releaseFree()      │  队列或释放        │
│  └───────────┬──────────────┘                    │
│              ▼                                    │
│         [循环继续]                                │
│                                                  │
│  退出时:                                         │
│  - unsubscribe() → 从数据源注销                  │
│  - removeWorker() → 从同步器移除                 │
│  - countDown → 通知 Group 线程                   │
└─────────────────────────────────────────────────┘
```

**关键代码路径** (退出逻辑，v2.70 新增)：

```cpp
// 文件: MultiWorkerProductionLine.cpp, workerThreadFunc 末尾
// Worker 退出时必须同步更新计数器
auto* connector = group->getConnectorForConsumer(consumer_name);
if (connector && connector->shared_source) {
    connector->shared_source->unsubscribe(consumer_info->worker.get());
}
size_t conn_idx = group->getConnectorIndex(connector);
if (conn_idx != SIZE_MAX) {
    auto it = group->connector_coordinators.find(conn_idx);
    if (it != group->connector_coordinators.end()) {
        it->second->removeWorker(consumer_name);
    }
}
```

### 2.5 performFrameSync() - 帧同步封装

```cpp
bool performFrameSync(
    const std::shared_ptr<WorkerGroupRuntime>& group,
    const std::string& consumer_name,
    WorkerGroupRuntime::ConsumerInfo* consumer_info,
    Buffer* buffer,
    const FillResult& result
);
```

如果该 Consumer 所属的 Connector 配置了 `enable_frame_sync = true`，则调用 `WorkerSyncCoordinator::arrive()`。

---

## 3. WorkerSyncCoordinator 核心函数

### 3.1 arrive() - Worker 到达同步点

```cpp
// 文件: components/source/productionline/line/WorkerSyncCoordinator.cpp:178
bool arrive(const std::string& worker_name, uint64_t frame_version, 
            Buffer* buffer, const FillResult& result);
```

**状态机逻辑：**

```
              ┌────────────────────────┐
              │   Worker 到达 arrive()  │
              └───────────┬────────────┘
                          ▼
              ┌────────────────────────┐
              │ 记录 worker_buffers    │
              │ 记录 worker_results    │
              │ arrived_count++        │
              └───────────┬────────────┘
                          ▼
            ┌─────────────────────────────┐
            │arrived_count == total_workers?│
            └─────┬──────────────┬────────┘
                  │ YES          │ NO
                  ▼              ▼
     ┌──────────────────┐  ┌──────────────────────────┐
     │ 分析状态组合      │  │ cv_.wait_for(5s)         │
     │ (5种场景)         │  │ 条件: callback_executed   │
     └────────┬─────────┘  │    OR arrived >= total    │
              ▼             └────────────┬─────────────┘
     ┌──────────────────┐               ▼
     │ 执行回调或缓存帧 │  ┌──────────────────────────┐
     │ cv_.notify_all() │  │ 超时: return false       │
     └──────────────────┘  │ 正常: return should_submit│
                           └──────────────────────────┘
```

**5 种同步场景：**

| 场景 | 条件 | 处理 |
|------|------|------|
| 1. 全部成功 | `success_count == total_workers` | 检查 PTS 一致性 → 执行回调 or 缓存 |
| 2a. 全部 EAGAIN | `eagain_count == total_workers` | 跳过，`should_submit = false` |
| 3a. 部分成功+部分 EAGAIN | `success > 0 && eagain > 0` | 深拷贝成功帧到 `pending_frames_` |
| 4. 有 EOF | `eof_count > 0` | 清空 pending，跳过 |
| 5. 有 ERROR | 其他 | 清空 pending，跳过 |

### 3.2 PTS 深拷贝匹配机制 (v2.70 核心创新)

```
场景: HW 解码器输出 B 帧重排序后 PTS 顺序，SW 解码器输出解码顺序

HW 输出: Frame(PTS=0), Frame(PTS=40), Frame(PTS=80), Frame(PTS=120)...
SW 输出: Frame(PTS=0), Frame(PTS=80), Frame(PTS=40), Frame(PTS=120)...
                            ↑              ↑
                        B帧先输出       I/P帧先输出

arrive() 第 N 次调用:
  HW: PTS=40 (SUCCESS)
  SW: EAGAIN          → 场景 3a: 深拷贝 HW Frame(PTS=40) 到 pending

arrive() 第 N+1 次调用:
  HW: PTS=80 (SUCCESS)
  SW: PTS=80 (SUCCESS) → 场景 1: PTS 一致，执行比较回调
  同时: tryMatchPending() → 发现 pending 中有 PTS=40 的 HW 帧

arrive() 第 N+2 次调用:
  HW: EAGAIN
  SW: PTS=40 (SUCCESS) → 场景 3a: 深拷贝 SW Frame(PTS=40) 到 pending
  tryMatchPending() → pending 中有 HW(PTS=40) 和 SW(PTS=40) → 匹配！执行比较
```

### 3.3 tryMatchPending() - PTS 匹配

```cpp
void tryMatchPending(const std::map<std::string, Buffer*>& current_buffers);
```

1. 按 PTS 将 `pending_frames_` 分组
2. 找到所有 Worker 都有帧的 PTS 组
3. 对匹配的帧调用 `comparator_->compareAVFrames(frame_a, frame_b)`
4. 释放已消费的帧，保留未匹配的
5. 防御：超过 64 帧未匹配则全部清空

### 3.4 removeWorker() - Worker 退出通知

```cpp
void removeWorker(const std::string& worker_name);
```

- 递减 `total_workers_`
- `cv_.notify_all()` 唤醒等待中的其他 Worker
- 使 `arrive()` 中的 `wait_for` 条件 `arrived_count >= total_workers_` 可能立即满足

### 3.5 createDefaultCompareCallback() - 比较回调工厂

静态方法，返回一个 `CallbackChainItem`，其内部 lambda：
1. 获取两个 Worker 的 Buffer
2. 使用 `BufferComparator::compare()` 计算 PSNR/SSIM
3. 累积统计到 `CompareCallbackContext`
4. 日志输出（失败时 WARN，verbose 时 DEBUG）

---

## 4. Bug 修复记录

### Bug 1: CS-009 色彩空间丢失

**问题**：`BufferComparator::convertToYUV420P()` 对 `YUVJ420P`（全范围 YUV）调用 `sws_scale` 时，默认将全范围 (0-255) 转为受限范围 (16-235)，导致 PSNR 从 50+ 降至 28。

**根因**：`sws_getContext` 默认色彩范围转换行为。

**修复**：对 `YUVJ420P`/`YUV420P` 直接 `av_frame_clone()` 跳过 `sws_scale`；对 NV12/NV21 手动去交织 U/V 平面。

```cpp
// 文件: components/source/consumptionline/BufferComparator.cpp
if (src_fmt == AV_PIX_FMT_YUVJ420P || src_fmt == AV_PIX_FMT_YUV420P) {
    return av_frame_clone(frame);  // 直接克隆，不经过 sws_scale
}
```

### Bug 2: B 帧 EAGAIN 不一致死锁

**问题**：HW 解码器设置 `reorder_disable=true` 导致输出为解码顺序（DTS），SW 解码器输出为显示顺序（PTS），帧顺序不对齐导致 `arrive()` 无法配对。

**根因**：`WorkerSyncCoordinator` 原设计假设同一 `frame_version` 时所有 Worker 同步完成，未考虑 B 帧重排序。

**修复**：
- VdecPlugin 中设置 `reorder_disable = false`（HW 也按 PTS 输出）
- 新增 `PendingFrame` 深拷贝缓存 + `tryMatchPending()` PTS 匹配

### Bug 3: Worker 退出后 busy loop

**问题**：Worker 异常退出后，`EncodedPacketSourceFromBuffer` 的 `total_subscribers_` 和 `WorkerSyncCoordinator` 的 `total_workers_` 未递减，导致存活 Worker 永远等不到 "全部到达"。

**根因**：缺少 Worker 退出通知机制。

**修复**：
- 新增 `EncodedPacketSourceFromBuffer::unsubscribe()`
- 新增 `WorkerSyncCoordinator::removeWorker()`
- `workerThreadFunc` 退出时调用两者
- `arrive()` 的 `wait_for` 条件增加 `arrived_count >= total_workers_`

### Bug 4: FFmpegDecodeWorker stale 指针

**问题**：`readAndSendPacket()` 在 `EAGAIN` 或错误路径未重置 `packet_acquired_` 和 `current_packet_ptr_`，导致下次调用时使用已释放的指针。

**修复**：所有非 SUCCESS 返回路径显式重置：
```cpp
packet_acquired_ = false;
current_packet_ptr_ = nullptr;
```

---

## 5. 数据流时序图

```
┌────────────┐  ┌────────────────────┐  ┌─────────────┐  ┌──────────────────┐
│ RecordWorker│  │EncodedPacketSource │  │ HW Decoder  │  │ SW Decoder       │
│(Producer)   │  │FromBuffer          │  │(Consumer 1) │  │(Consumer 2)      │
└─────┬──────┘  └─────────┬──────────┘  └──────┬──────┘  └────────┬─────────┘
      │                    │                     │                   │
      │ produce packet     │                     │                   │
      │───────────────────▶│                     │                   │
      │                    │ acquirePacket()      │                   │
      │                    │◀────────────────────│                   │
      │                    │                     │                   │
      │                    │ acquirePacket()      │                   │
      │                    │◀───────────────────────────────────────│
      │                    │                     │                   │
      │                    │ (wait all acquired) │                   │
      │                    │────advance version──▶│                   │
      │                    │                     │                   │
      │                    │                     │ fillBuffer()      │
      │                    │                     │──────┐            │
      │                    │                     │      │ decode     │
      │                    │                     │◀─────┘            │
      │                    │                     │                   │ fillBuffer()
      │                    │                     │                   │──────┐
      │                    │                     │                   │      │
      │                    │                     │                   │◀─────┘
      │                    │                     │                   │
      │                    │              ┌──────┴───────────────────┴──────┐
      │                    │              │    WorkerSyncCoordinator         │
      │                    │              │    arrive(HW, buf_hw, SUCCESS)   │
      │                    │              │    arrive(SW, buf_sw, SUCCESS)   │
      │                    │              │    → executeCallbackChain()      │
      │                    │              │    → compare PSNR/SSIM           │
      │                    │              └─────────────────────────────────┘
```
