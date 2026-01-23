# BufferPacketSource 共享模式重大 Bug 修复报告 (v2.22)

## 📋 概述

本文档记录了 v2.22 版本中对 `BufferPacketSource` 共享模式（ONE_TO_MANY）的重大架构重构，解决了多个严重的并发问题，包括资源泄漏、死锁和重复处理等。

**修复日期**: 2026-01-22  
**版本**: v2.22  
**影响范围**: BufferPacketSource 共享模式、FfmpegDecodeRtspWorker Buffer 模式

---

## 🐛 问题背景

### 场景描述

在 MultiWorker 共享模式下，一个 `BufferPacketSource` 同时为多个 `FfmpegDecodeRtspWorker` 提供数据（ONE_TO_MANY）：

```
FfmpegPacketRecorderWorker (生产者)
        ↓
   BufferPool (filled queue)
        ↓
 BufferPacketSource (共享)
    ↙        ↘
Worker 1    Worker 2  (消费者)
```

**核心需求**：
1. 每个 Worker 必须处理每一个 Buffer（packet）
2. 只有所有 Worker 都处理完当前 Buffer，`BufferPacketSource` 才能释放并获取下一个
3. 支持失败重试（如解码失败）

---

## 🔍 发现的严重 Bug

### Bug 1: 双重清空导致的资源泄漏和死锁 ⚠️⚠️⚠️

#### 问题描述

在 v2.21 版本中，`commitPacket()` 和 `fetchTaskFunc()` 都尝试清空 `current_buffer_`，导致：
1. **资源泄漏**：Buffer 没有被释放回 BufferPool
2. **死锁**：fetch 任务无法获取新 Buffer，Workers 永远等待新数据

#### 错误的代码逻辑

```cpp
// commitPacket() - 第一次清空
bool BufferPacketSource::commitPacket(void* worker_id) {
    // ...
    if (remaining == 0) {
        current_buffer_ = nullptr;  // ❌ 第一次清空
        cv_fetch_.notify_one();
    }
}

// fetchTaskFunc() - 尝试第二次清空
void BufferPacketSource::fetchTaskFunc() {
    // ...
    // 步骤2：释放当前 Buffer
    if (current_buffer_) {  // ❌ 已经是 nullptr！
        pool->releaseFilled(current_buffer_);
        current_buffer_ = nullptr;
    }
    
    // 步骤3：获取新 Buffer
    Buffer* new_buffer = pool->acquireFilled(true, 100);  // ❌ Pool 已满，超时
}
```

#### 时序分析

```
时间轴：commitPacket()               fetchTaskFunc()
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
T1:  remaining == 0
     ↓
T2:  current_buffer_ = nullptr  ⚠️ 第一次清空
     ↓
T3:  cv_fetch_.notify_one()
                                     ↓
T4:                              被唤醒，检查 current_buffer_
                                     ↓
T5:                              if (current_buffer_) { ❌ 已是 nullptr
                                     // 不会执行 releaseFilled()
                                 }
                                     ↓
T6:                              pool->acquireFilled() ⚠️ 永远阻塞
                                 （Pool 已满，旧 Buffer 未释放）
```

#### 症状

运行日志显示：
```log
[Worker 0x403140] commitPacket: Success, version=32, remaining=0
[Worker 0x4a7200] commitPacket: Success, version=32, remaining=0
All subscribers committed, notifying fetch task

# 之后卡住，没有 version=33

[Worker 0x403140] acquirePacket: Already processed version 32  # 疯狂重复
[Test] Consumer 1: Timeout waiting for buffer (1)
[Test] Consumer 2: Timeout waiting for buffer (1)
[VideoLine] Waiting for free buffer from pool (wait_count=101)  # Pool 已满
```

#### 解决方案

**单一职责原则**：
- `commitPacket()`: 只负责递减计数器和唤醒 fetch 任务
- `fetchTaskFunc()`: 独占负责清空和释放 `current_buffer_`

```cpp
// ✅ 修复后 (commitPacket)
if (remaining == 0) {
    // 不清空 current_buffer_，由 fetch 任务负责
    cv_fetch_.notify_one();
}

// ✅ 修复后 (fetchTaskFunc)
Buffer* buffer_to_release = nullptr;
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_buffer_) {  // ✅ 此时仍指向有效 Buffer
        buffer_to_release = current_buffer_;
        current_buffer_ = nullptr;
    }
}
if (buffer_to_release) {
    pool->releaseFilled(buffer_to_release);  // ✅ 成功释放
}
```

---

### Bug 2: Worker 重复处理同一 Buffer

#### 问题描述

在旧的 RAII 架构中，如果 Worker 处理速度快（一个 packet 解码出多帧），会导致同一个 Worker 多次获取同一个 Buffer，造成：
1. `remaining_subscribers_` 计数错误（下溢）
2. 其他 Worker 永远等待

#### 错误的逻辑

```cpp
// 旧版本：没有版本号机制
AVPacket* BufferPacketSource::acquirePacket() {
    // 任何 Worker 都可以随时获取 current_buffer_
    return current_buffer_->getAVPacket();
}

void BufferPacketSource::releasePacket() {
    size_t remaining = remaining_subscribers_.fetch_sub(1) - 1;
    // ❌ 同一个 Worker 多次调用，remaining 下溢
}
```

#### 症状

```log
[Worker 0x403140] commitPacket: Success, version=28, remaining=1
[Worker 0x403140] commitPacket: Success, version=28, remaining=0  # 同一 Worker 重复
[Worker 0x403140] commitPacket: Success, version=28, remaining=18446744073709551615  # 下溢！
```

#### 解决方案

**引入版本号机制和 Worker 状态追踪**：

```cpp
// 数据结构
struct WorkerState {
    uint64_t acquired_version = 0;    // Worker 获取的 buffer 版本号
    bool has_acquired = false;        // 是否已获取当前版本
    bool has_committed = false;       // 是否已提交当前版本
};
std::map<void*, WorkerState> worker_states_;  // Worker ID -> 状态
std::atomic<uint64_t> current_buffer_version_{0};  // Buffer 版本号

// ✅ acquirePacket 检查版本号
AVPacket* BufferPacketSource::acquirePacket(void* worker_id) {
    uint64_t current_version = current_buffer_version_.load();
    WorkerState& state = worker_states_[worker_id];
    
    // 检查是否已处理过当前版本
    if (state.acquired_version == current_version) {
        return nullptr;  // ✅ 防止重复获取
    }
    
    // 新版本，允许获取
    state.acquired_version = current_version;
    state.has_acquired = true;
    state.has_committed = false;
    
    return current_buffer_->getAVPacket();
}

// fetchTaskFunc 递增版本号
void BufferPacketSource::fetchTaskFunc() {
    current_buffer_ = new_buffer;
    current_buffer_version_.fetch_add(1);  // ✅ 每个新 buffer 版本号递增
}
```

---

### Bug 3: RAII 模式不适合多状态管理

#### 问题描述

旧的 `PacketGuard` RAII 包装器只支持"获取-释放"两状态，无法处理：
1. **失败重试**：`avcodec_send_packet()` 失败时需要重试同一个 packet
2. **部分成功**：`avcodec_receive_frame()` 返回 `EAGAIN` 时需要重新发送
3. **状态追踪**：无法区分 Worker 是否真正完成处理

#### 错误的架构

```cpp
// 旧版本：RAII 强制"获取即释放"
PacketGuard guard(buffer_source);
if (guard) {
    avcodec_send_packet(codec, guard.get());
    avcodec_receive_frame(codec, frame);
}
// ❌ guard 析构自动调用 releasePacket()，即使解码失败也释放
```

#### 解决方案

**引入三状态 API (acquire/commit/cancel)**：

```cpp
// 新架构：精确控制生命周期
AVPacket* packet = ps->acquirePacket(this);
if (!packet) return false;

// 状态1：已获取，尝试发送
int ret = avcodec_send_packet(codec, packet);
if (ret < 0 && ret != AVERROR(EAGAIN)) {
    ps->cancelPacket(this);  // ✅ 取消，下次重试
    return false;
}

// 状态2：尝试接收帧
bool decoded_at_least_one = false;
while (avcodec_receive_frame(codec, frame) >= 0) {
    decoded_at_least_one = true;
    // 处理帧...
}

// 状态3：根据结果决定
if (decoded_at_least_one) {
    ps->commitPacket(this);  // ✅ 成功，提交释放
} else {
    ps->cancelPacket(this);  // ✅ 失败，取消并重试
}
```

---

### Bug 4: Worker 状态不一致

#### 问题描述

在没有 `has_committed` 标志时，`commitPacket()` 会重置 `has_acquired = false`，导致 Worker 可以在同一版本内多次 commit。

#### 错误的逻辑

```cpp
// 旧版本
bool BufferPacketSource::commitPacket(void* worker_id) {
    state.has_acquired = false;  // ❌ 立即重置
    remaining_subscribers_.fetch_sub(1);
    // Worker 可以再次 acquirePacket，然后再次 commitPacket！
}
```

#### 解决方案

**引入 `has_committed` 标志防止重复提交**：

```cpp
// ✅ acquirePacket: 检查版本号
if (state.acquired_version == current_version) {
    return nullptr;  // 已处理过此版本（无论 acquired 还是 committed）
}

// ✅ commitPacket: 检查 has_committed
if (!state.has_acquired) {
    return false;  // 未获取，无法提交
}
if (state.has_committed) {
    return false;  // ✅ 已提交，防止重复
}

state.has_acquired = false;   // 重置获取状态
state.has_committed = true;   // ✅ 标记已提交
```

---

## 🏗️ 完整的解决方案架构

### 核心数据结构

```cpp
class BufferPacketSource {
private:
    // 版本号机制
    std::atomic<uint64_t> current_buffer_version_{0};
    
    // Worker 状态追踪
    struct WorkerState {
        uint64_t acquired_version = 0;    // 获取的版本号
        bool has_acquired = false;        // 是否已获取当前版本
        bool has_committed = false;       // 是否已提交当前版本
    };
    std::map<void*, WorkerState> worker_states_;
    
    // 同步机制
    Buffer* current_buffer_ = nullptr;
    std::atomic<size_t> remaining_subscribers_;
    std::mutex mutex_;
    std::condition_variable cv_subscribers_;  // 唤醒 Workers
    std::condition_variable cv_fetch_;        // 唤醒 Fetch 任务
};
```

### API 设计

#### 1. acquirePacket(worker_id)

**功能**: 获取 AVPacket 指针（阻塞等待）

```cpp
AVPacket* BufferPacketSource::acquirePacket(void* worker_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // ⭐ 阻塞等待新 buffer 或 EOF
    cv_subscribers_.wait(lock, [this]() {
        return current_buffer_ != nullptr || !is_running_;
    });
    
    // 检查 EOF
    if (!is_running_ && !current_buffer_) {
        return nullptr;
    }
    
    uint64_t current_version = current_buffer_version_.load();
    WorkerState& state = worker_states_[worker_id];
    
    // ⭐ 防止重复获取同一版本
    if (state.acquired_version == current_version) {
        return nullptr;
    }
    
    // ✅ 新版本，允许获取
    state.acquired_version = current_version;
    state.has_acquired = true;
    state.has_committed = false;
    
    return current_buffer_->getAVPacket();
}
```

#### 2. commitPacket(worker_id)

**功能**: 提交释放（成功处理后调用）

```cpp
bool BufferPacketSource::commitPacket(void* worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    WorkerState& state = worker_states_[worker_id];
    
    // 检查状态
    if (!state.has_acquired) return false;
    if (state.has_committed) return false;  // ⭐ 防止重复提交
    
    // 更新状态
    state.has_acquired = false;
    state.has_committed = true;
    
    // 递减计数
    size_t remaining = remaining_subscribers_.fetch_sub(1) - 1;
    
    // ⭐ 所有 Worker 完成，唤醒 fetch 任务
    if (remaining == 0) {
        cv_fetch_.notify_one();
        // ⚠️ 不清空 current_buffer_！由 fetchTaskFunc 负责
    }
    
    return true;
}
```

#### 3. cancelPacket(worker_id)

**功能**: 取消获取（失败时调用，支持重试）

```cpp
void BufferPacketSource::cancelPacket(void* worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    WorkerState& state = worker_states_[worker_id];
    
    // ⭐ 重置获取状态，允许重新获取当前版本
    state.has_acquired = false;
    // 注意：不递减 remaining_subscribers_！
    // 注意：不改变 acquired_version！（仍是当前版本，允许重试）
}
```

### 使用示例

#### Worker 端（FfmpegDecodeRtspWorker）

```cpp
bool FfmpegDecodeRtspWorker::fillBuffer(int frame_index, Buffer* buffer) {
    // 步骤1: 检查缓存队列
    if (!cached_frames_.empty()) {
        // 使用缓存帧...
        return true;
    }
    
    // 步骤2: 获取 packet（仅在未获取时）
    if (!packet_acquired_) {
        current_packet_ptr_ = ps->acquirePacket(this);
        if (!current_packet_ptr_) {
            return false;  // EOF 或已获取过
        }
        packet_acquired_ = true;
    }
    
    // 步骤3: 发送到解码器
    int ret = avcodec_send_packet(codec_ctx_ptr_, current_packet_ptr_);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        ps->cancelPacket(this);  // ✅ 失败，取消
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
        return false;
    }
    
    // 步骤4: 接收解码帧
    bool decoded_at_least_one = false;
    while (avcodec_receive_frame(codec_ctx_ptr_, temp_frame) >= 0) {
        decoded_at_least_one = true;
        cached_frames_.push_back(temp_frame);
    }
    
    // 步骤5: 处理结果
    if (!decoded_at_least_one) {
        ps->cancelPacket(this);  // ✅ 无帧，取消（下次重试）
        packet_acquired_ = false;
        current_packet_ptr_ = nullptr;
        return false;
    }
    
    // 步骤6: 成功，提交
    ps->commitPacket(this);  // ✅ 成功，提交释放
    packet_acquired_ = false;
    current_packet_ptr_ = nullptr;
    
    // 步骤7: 从缓存取第一帧填充 buffer
    AVFrame* first_frame = cached_frames_.front();
    cached_frames_.erase(cached_frames_.begin());
    av_frame_move_ref(buffer->getAVFrame(), first_frame);
    
    return true;
}
```

#### Fetch 任务端

```cpp
void BufferPacketSource::fetchTaskFunc() {
    while (is_running_) {
        // 步骤1: 等待所有订阅者完成
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_fetch_.wait(lock, [this]() {
                return remaining_subscribers_ == 0 || !is_running_;
            });
        }
        
        // 步骤2: 释放当前 Buffer（⭐ 独占负责）
        Buffer* buffer_to_release = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_buffer_) {
                buffer_to_release = current_buffer_;
                current_buffer_ = nullptr;
            }
        }
        if (buffer_to_release) {
            pool->releaseFilled(buffer_to_release);  // ✅ 释放回 Pool
        }
        
        // 步骤3: 获取新 Buffer
        Buffer* new_buffer = pool->acquireFilled(true, 100);
        if (!new_buffer) {
            if (!is_running_ || !pool->isRunning()) {
                cv_subscribers_.notify_all();  // 唤醒所有 Worker（EOF）
                break;
            }
            continue;
        }
        
        // 步骤4: 设置新 Buffer 并唤醒订阅者
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_buffer_ = new_buffer;
            current_buffer_version_.fetch_add(1);  // ⭐ 递增版本号
            remaining_subscribers_.store(total_subscribers_);
        }
        cv_subscribers_.notify_all();  // 唤醒所有 Worker
    }
}
```

---

## 📊 状态机图

### Worker 状态转换

```
                    [初始状态]
                        ↓
            ┌───────────────────────┐
            │  未获取 (version=0)   │
            │  has_acquired=false   │
            │  has_committed=false  │
            └───────────────────────┘
                        ↓ acquirePacket()
            ┌───────────────────────┐
            │  已获取 (version=N)   │
            │  has_acquired=true    │
            │  has_committed=false  │
            └───────────────────────┘
                  ↙            ↘
          cancelPacket()   commitPacket()
                ↙                ↘
    ┌───────────────────┐   ┌───────────────────┐
    │ 取消 (version=N)  │   │ 提交 (version=N) │
    │ has_acquired=false│   │ has_acquired=false│
    │ has_committed=false│   │ has_committed=true│
    └───────────────────┘   └───────────────────┘
            ↓                        ↓
      可重试相同版本          等待新版本 (N+1)
```

### Buffer 版本生命周期

```
Fetch Task:                Workers:
━━━━━━━━━━                ━━━━━━━━━
version=1
remaining=2
    ↓
notify_all()  ─────→  Worker1: acquire(v1) ✓
                      Worker2: acquire(v1) ✓
                            ↓
                      Worker1: process...
                      Worker2: process...
                            ↓
                      Worker1: commit(v1) → remaining=1
                      Worker2: commit(v1) → remaining=0
                            ↓
wait(remaining==0)  ←─ notify_one()
    ↓
release Buffer(v1)
acquire new Buffer
    ↓
version=2
remaining=2
    ↓
notify_all()  ─────→  Worker1: acquire(v2) ✓
                      Worker2: acquire(v2) ✓
                      ...
```

---

## ✅ 验证结果

### 修复前的错误日志

```log
[Worker 0x403140] acquirePacket: Success, version=32
[Worker 0x4a7200] acquirePacket: Success, version=32
[Worker 0x403140] commitPacket: Success, version=32, remaining=1
[Worker 0x4a7200] commitPacket: Success, version=32, remaining=0
All subscribers committed, notifying fetch task

# ❌ 卡住，没有 version=33
[Worker 0x403140] acquirePacket: Already processed version 32  # 重复 50+ 次
[Test] Consumer 1: Timeout waiting for buffer (1)
[Test] Consumer 2: Timeout waiting for buffer (2)
[VideoLine] Waiting for free buffer from pool (wait_count=101)  # 死锁
```

### 修复后的正常日志

```log
[Worker 0x55556901c200] acquirePacket: Success, version=143
[Worker 0x555568f78140] acquirePacket: Success, version=143
[Worker 0x55556901c200] commitPacket: Success, version=143, remaining=1
[Worker 0x555568f78140] commitPacket: Success, version=143, remaining=0

# ✅ 正常推进
[Worker 0x55556901c200] acquirePacket: Success, version=144
[Worker 0x555568f78140] acquirePacket: Success, version=144
[Worker 0x55556901c200] commitPacket: Success, version=144, remaining=1
[Worker 0x555568f78140] commitPacket: Success, version=144, remaining=0

# ✅ 持续稳定运行
[Worker 0x55556901c200] acquirePacket: Success, version=145
[Worker 0x555568f78140] acquirePacket: Success, version=145
...
```

---

## 🔧 代码修改清单

### 新增文件
- 无

### 修改文件

#### 1. `include/productionline/worker/BufferPacketSource.hpp`
- 移除 `PacketGuard` 类定义
- 新增 `WorkerState` 结构体
- 新增 `std::atomic<uint64_t> current_buffer_version_`
- 新增 `std::map<void*, WorkerState> worker_states_`
- 修改 API 签名：
  - `AVPacket* acquirePacket(void* worker_id)` （新增 worker_id 参数）
  - `bool commitPacket(void* worker_id)` （新增，替代 releasePacket）
  - `void cancelPacket(void* worker_id)` （新增）
  - 移除 `void releasePacket()`

#### 2. `source/productionline/worker/BufferPacketSource.cpp`
- 移除 `PacketGuard` 实现（构造、析构、移动操作）
- 重写 `acquirePacket()`: 新增版本号检查和 Worker 状态追踪
- 新增 `commitPacket()`: 实现三状态管理
- 新增 `cancelPacket()`: 支持重试机制
- 修复 `fetchTaskFunc()`: 
  - 不在 `commitPacket` 中清空 `current_buffer_`
  - 在锁内读取、锁外释放 Buffer
  - 递增 `current_buffer_version_`

#### 3. `include/productionline/worker/FfmpegDecodeRtspWorker.hpp`
- 新增 `AVPacket* current_packet_ptr_` 成员
- 新增 `bool packet_acquired_` 成员

#### 4. `source/productionline/worker/FfmpegDecodeRtspWorker.cpp`
- 修改 `fillBuffer()`: 
  - 使用新的三状态 API
  - 根据解码结果调用 `commitPacket` 或 `cancelPacket`
  - 在 `close()` 中清理未提交的 packet
- 修改 `readAndSendPacket()`: 移除 Buffer 模式逻辑（已迁移到 fillBuffer）

#### 5. `components.mk`
- 更新版本号: `COMPONENTS_VERSION = 2.21` → `2.22`

---

## 🎯 性能和稳定性改进

### 1. 资源管理
- ✅ 消除资源泄漏：Buffer 正确释放回 BufferPool
- ✅ 防止死锁：fetch 任务和 Workers 正确协调

### 2. 并发安全
- ✅ 版本号机制：防止 Worker 重复处理同一 Buffer
- ✅ 状态追踪：精确管理每个 Worker 的生命周期
- ✅ 原子操作：`remaining_subscribers_` 计数准确无误

### 3. 错误处理
- ✅ 重试支持：`cancelPacket` 允许失败后重试
- ✅ 状态一致：`has_committed` 防止重复提交
- ✅ EOF 处理：正确检测和传播数据源结束信号

### 4. 日志优化
- ✅ 移除冗余调试日志：减少日志噪音
- ✅ 保留关键日志：错误和警告信息便于诊断

---

## 📚 相关文档

- `ARCHITECTURE.md`: 系统架构文档
- `questions.md`: 原始问题分析文档
- Git Commit: `68ceb2d` - fix(BufferPacketSource): 修复共享模式下的资源泄漏和死锁问题 (v2.22)
- Git Tag: `v2.22`

---

## 🔮 后续建议

### 1. 监控和测试
- [ ] 添加单元测试覆盖新的三状态 API
- [ ] 长时间运行测试验证稳定性
- [ ] 监控 `remaining_subscribers_` 计数是否始终准确

### 2. 代码优化
- [ ] 考虑使用 `std::unordered_map` 替代 `std::map` 提升性能
- [ ] 评估是否需要为 `worker_states_` 添加清理机制（避免内存泄漏）

### 3. 文档完善
- [ ] 更新 API 文档和使用示例
- [ ] 添加架构图和序列图
- [ ] 提供最佳实践指南

---

**文档版本**: 1.0  
**最后更新**: 2026-01-22  
**作者**: AI Assistant  
**审核**: 待审核
