# BufferPool 所有权和使用需求分析

## 📊 所有涉及 BufferPool 的组件汇总表

| 组件 | 角色 | 创建 | 拥有 | 使用 | 操作类型 | 具体操作 | 权限 |
|------|------|------|------|------|----------|----------|------|
| **Allocator** | 创建者 + 持有者 | ✅ 是 | ✅ 是<br>（持有 shared_ptr） | ✅ 是 | 写操作 | `allocatePoolWithBuffers()`<br>`injectBufferToPool()`<br>`removeBufferFromPool()`<br>`destroyPool()` | 写权限（管理 Buffer 生命周期） |
| **Worker** | 创建者（通过 Allocator） | ✅ 是<br>（通过 Allocator） | ❌ 否<br>（临时持有 shared_ptr，创建后通过 getOutputBufferPool() 转换） | ✅ 是<br>（只用于填充 buffer） | 读操作 | `getOutputBufferPool()`<br>`fillBuffer()` | 无（创建后不再需要） |
| **ProductionLine** | 队列管理者 | ❌ 否 | ✅ 是<br>（持有 unique_ptr，从 Worker 获取） | ✅ 是 | 写操作 | `acquireFree()`<br>`submitFilled()`<br>管理队列状态 | 写权限（管理队列） |
| **Display** | 使用者 | ❌ 否 | ❌ 否 | ✅ 是 | 读操作 | `getBufferPool()`<br>`displayBuffer()`<br>`getBufferCount()`<br>`getBufferSize()` | 只读权限 |
| **Consumer** | 使用者 | ❌ 否 | ❌ 否 | ✅ 是 | 读操作 | `acquireFilled()`<br>`releaseFilled()`<br>`getFreeCount()`<br>`getFilledCount()` | 只读权限（队列操作） |
| **BufferPoolRegistry** | 统一管理器 | ❌ 否 | ✅ 是<br>（持有 shared_ptr，统一管理） | ✅ 是 | 只读操作 | `registerPool()`<br>`unregisterPool()`<br>`getPoolReadOnly()`<br>`getPoolByNameForProductionLine()`<br>`getAllPoolsReadOnly()`<br>`printAllStats()` | 只读权限（查询和统计）<br>读写权限（仅 ProductionLine 通过 friend） |

## 🔍 详细分析

### 1. Allocator（创建者 + 持有者 + 写权限）

**职责**：
- ✅ **创建 BufferPool**：通过 `allocatePoolWithBuffers()` 创建空的 BufferPool
- ✅ **持有 BufferPool**：通过 `managed_pools_` 持有所有创建的 BufferPool 的 `shared_ptr`
- ✅ **注入 Buffer**：通过 `injectBufferToPool()` 将 Buffer 注入到 BufferPool
- ✅ **移除 Buffer**：通过 `removeBufferFromPool()` 从 BufferPool 移除 Buffer
- ✅ **销毁 BufferPool**：通过 `destroyPool()` 销毁整个 BufferPool

**所有权**：
- ✅ **持有 BufferPool**：通过 `std::vector<std::shared_ptr<BufferPool>> managed_pools_` 持有所有创建的 BufferPool
- ✅ **管理生命周期**：Allocator 持有 `shared_ptr`，确保 BufferPool 在使用期间不被销毁
- ✅ **有写权限**：通过友元关系可以访问 BufferPool 的私有方法

**使用场景**：
- Worker 在 `open()` 时调用 Allocator 创建 BufferPool
- Worker 在解码循环中调用 Allocator 注入 Buffer（动态注入模式）
- Allocator 析构时自动清理所有创建的 BufferPool

**实现细节**：
```cpp
class BufferAllocatorBase {
protected:
    std::vector<std::shared_ptr<BufferPool>> managed_pools_;  // 持有所有创建的 BufferPool
    mutable std::mutex managed_pools_mutex_;                  // 保护 managed_pools_
    
public:
    std::shared_ptr<BufferPool> allocatePoolWithBuffers(...) {
        auto pool = BufferPool::CreateEmpty(name, category);
        // ... 创建 Buffer 并注入 ...
        
        // Allocator 持有 shared_ptr
        {
            std::lock_guard<std::mutex> lock(managed_pools_mutex_);
            managed_pools_.push_back(pool);
        }
        
        // 自动注册到 Registry
        BufferPoolRegistry::getInstance().registerPool(pool, name, category);
        
        return pool;
    }
};
```

### 2. Worker（创建者，不持有）

**职责**：
- ✅ **创建 BufferPool**：在 `open()` 时通过调用 Allocator 创建 BufferPool
- ✅ **填充 Buffer**：通过 `fillBuffer()` 填充 Buffer（核心功能）
- ❌ **不管理 BufferPool**：创建后通过 `getOutputBufferPool()` 转换给 ProductionLine

**所有权**：
- ❌ **不持有 BufferPool**：创建后立即通过 `getOutputBufferPool()` 转换给 ProductionLine
- ✅ **临时持有**：在 `open()` 时临时持有 `shared_ptr`，用于访问 BufferPool 信息（如 `getName()`）

**使用场景**：
- Worker 在 `open()` 时创建 BufferPool（通过 Allocator）
- Worker 通过 `getOutputBufferPool()` 返回给 ProductionLine（转换为 `unique_ptr`）
- Worker 后续只负责填充 buffer，不管理 BufferPool

**实现细节**：
```cpp
class WorkerBase {
protected:
    std::shared_ptr<BufferPool> buffer_pool_;  // 临时持有
    
public:
    virtual bool open(const char* path) override {
        // 调用 Allocator 创建 BufferPool（返回 shared_ptr）
        buffer_pool_ = allocator_.allocatePoolWithBuffers(...);
        return buffer_pool_ != nullptr;
    }
    
    virtual std::unique_ptr<BufferPool> getOutputBufferPool() override {
        if (!buffer_pool_) {
            return nullptr;
        }
        // 从 shared_ptr 转换为 unique_ptr
        // 注意：Allocator 和 Registry 仍持有 shared_ptr，所以不会销毁
        BufferPool* raw_ptr = buffer_pool_.get();
        buffer_pool_.reset();  // Worker 不再持有
        return std::unique_ptr<BufferPool>(raw_ptr);
    }
};
```

### 3. ProductionLine（队列管理者，写权限）

**职责**：
- ✅ **拥有 BufferPool**：持有 `std::unique_ptr<BufferPool> worker_buffer_pool_`
- ✅ **管理队列**：通过 `working_buffer_pool_` 指针管理 BufferPool 的队列
- ✅ **写操作**：调用 `acquireFree()` 和 `submitFilled()` 管理队列状态

**所有权**：
- ✅ **最终拥有者**：通过 `getOutputBufferPool()` 从 Worker 获取所有权（`unique_ptr`）
- ✅ **生命周期管理**：负责 BufferPool 的生命周期（但 Allocator 和 Registry 也持有 `shared_ptr`）

**使用场景**：
- 生产者线程调用 `acquireFree()` 获取空闲 Buffer
- 生产者线程调用 `submitFilled()` 提交填充后的 Buffer
- 对外提供 `getWorkingBufferPool()` 供消费者使用

**实现细节**：
```cpp
class VideoProductionLine {
private:
    std::unique_ptr<BufferPool> worker_buffer_pool_;  // 持有 unique_ptr
    
public:
    bool start(const Config& config) {
        // 从 Worker 获取 BufferPool（转换为 unique_ptr）
        worker_buffer_pool_ = worker_->getOutputBufferPool();
        working_buffer_pool_ = worker_buffer_pool_.get();
        // ...
    }
    
    void producerThreadFunc(int thread_id) {
        // 管理队列（写权限）
        Buffer* buf = worker_buffer_pool_->acquireFree(true, 100);
        worker_->fillBuffer(frame_index, buf);
        worker_buffer_pool_->submitFilled(buf);
    }
};
```

### 4. Display（使用者，只读权限）

**职责**：
- ✅ **访问 BufferPool**：通过 `getBufferPool()` 获取 BufferPool 指针
- ✅ **显示 Buffer**：通过 `displayBuffer()` 显示 Buffer
- ✅ **查询信息**：通过 `getBufferCount()`、`getBufferSize()` 查询信息

**所有权**：
- ❌ **不拥有 BufferPool**：只持有原始指针 `BufferPool* buffer_pool_`
- ✅ **只读权限**：只能读取，不能修改 BufferPool 的内部状态

**使用场景**：
- Display 通过 `setBufferPool()` 接收 BufferPool 指针
- Display 通过 `displayBuffer()` 显示 Buffer
- Display 通过 `getBufferPool()` 返回 BufferPool 指针供外部查询

### 5. Consumer（使用者，只读权限）

**职责**：
- ✅ **获取 Buffer**：通过 `acquireFilled()` 获取填充后的 Buffer
- ✅ **归还 Buffer**：通过 `releaseFilled()` 归还 Buffer
- ✅ **查询状态**：通过 `getFreeCount()`、`getFilledCount()` 查询状态

**所有权**：
- ❌ **不拥有 BufferPool**：只通过指针访问
- ✅ **只读权限**：只能读取和归还 Buffer，不能修改 BufferPool 的内部状态

**使用场景**：
- Consumer 从 ProductionLine 获取 `getWorkingBufferPool()` 指针
- Consumer 循环调用 `acquireFilled()` 和 `releaseFilled()`

### 6. BufferPoolRegistry（统一管理器，只读权限）

**职责**：
- ✅ **注册 BufferPool**：在 BufferPool 构造时自动注册
- ✅ **注销 BufferPool**：在 BufferPool 析构时自动注销
- ✅ **查询 BufferPool**：通过名称、分类查询 BufferPool
- ✅ **统计信息**：提供全局统计信息

**所有权**：
- ✅ **持有 BufferPool**：存储 `std::shared_ptr<BufferPool>`，统一管理所有 BufferPool
- ✅ **只读权限**：只能查询和统计，不能修改 BufferPool
- ✅ **读写权限**：仅 ProductionLine 通过 friend 可以调用读写接口

**使用场景**：
- BufferPool 构造时自动注册到 Registry
- 外部通过 Registry 查询和监控所有 BufferPool
- ProductionLine 通过 Registry 获取 BufferPool（读写版本）

**实现细节**：
```cpp
class BufferPoolRegistry {
private:
    std::unordered_map<uint64_t, PoolInfo> pools_;  // PoolInfo 包含 shared_ptr<BufferPool>
    
public:
    // 只读接口（公开）
    std::shared_ptr<const BufferPool> getPoolReadOnly(uint64_t id) const;
    std::shared_ptr<const BufferPool> getPoolReadOnlyByName(const std::string& name) const;
    
    // 读写接口（仅 ProductionLine 可以调用）
    std::shared_ptr<BufferPool> getPoolForProductionLine(uint64_t id);
    std::shared_ptr<BufferPool> getPoolByNameForProductionLine(const std::string& name);
    
private:
    friend class VideoProductionLine;  // 权限控制
};
```

## 🎯 最终所有权归属表

| 组件 | 所有权方式 | 持有类型 | 权限 | 生命周期管理 | 说明 |
|------|-----------|---------|------|------------|------|
| **Allocator** | ✅ 持有 | `std::shared_ptr<BufferPool>`<br>（存储在 `managed_pools_` 中） | 写权限 | 管理 BufferPool 的生命周期 | Allocator 持有所有创建的 BufferPool，确保在使用期间不被销毁 |
| **BufferPoolRegistry** | ✅ 持有 | `std::shared_ptr<BufferPool>`<br>（存储在 `pools_` 中） | 只读权限（查询）<br>读写权限（仅 ProductionLine） | 统一管理所有 BufferPool | Registry 持有所有 BufferPool，提供统一查询接口 |
| **Worker** | ❌ 不持有 | 临时持有 `std::shared_ptr<BufferPool>`<br>（创建后立即转换） | 无 | 不管理生命周期 | Worker 创建 BufferPool 后立即转换给 ProductionLine，不再持有 |
| **ProductionLine** | ✅ 持有 | `std::unique_ptr<BufferPool>`<br>（从 Worker 获取） | 写权限（管理队列） | 管理 BufferPool 的使用 | ProductionLine 持有 BufferPool，负责队列管理，但 Allocator 和 Registry 也持有 shared_ptr |
| **Display** | ❌ 不持有 | `BufferPool*`<br>（原始指针，不拥有所有权） | 只读权限 | 不管理生命周期 | Display 只持有指针，用于访问 BufferPool 信息 |
| **Consumer** | ❌ 不持有 | `BufferPool*`<br>（原始指针，不拥有所有权） | 只读权限（队列操作） | 不管理生命周期 | Consumer 只持有指针，用于队列操作 |

## 💡 最终解决方案：Allocator 持有 + Registry 统一管理 + ProductionLine 使用

### 核心设计

```
Worker::open()
    ↓
调用 Allocator::allocatePoolWithBuffers()
    ↓
Allocator 创建 BufferPool（返回 shared_ptr）
    ↓
Allocator 持有 shared_ptr<BufferPool>（管理生命周期）
    ↓
BufferPool 自动注册到 Registry（Registry 持有 shared_ptr，统一管理）
    ↓
Worker 临时持有 shared_ptr（用于访问信息）
    ↓
Worker 通过 getOutputBufferPool() 转换为 unique_ptr 返回给 ProductionLine
    ↓
ProductionLine 持有 unique_ptr<BufferPool>（管理队列，写权限）
    ↓
用户/Display/Consumer 通过 Registry 获取 BufferPool（只读）
```

### 职责划分

| 组件 | 职责 | 操作 | 所有权 | 权限 |
|------|------|------|--------|------|
| **Worker** | 创建 BufferPool（通过 Allocator）<br>填充 Buffer | `open()` 时调用 Allocator 创建<br>`fillBuffer()` 填充数据 | ❌ 不持有（创建后立即转换） | 无（创建后不再需要） |
| **Allocator** | 创建 BufferPool<br>管理 Buffer 生命周期 | `allocatePoolWithBuffers()`<br>`injectBufferToPool()`<br>`removeBufferFromPool()`<br>`destroyPool()` | ✅ 持有 `std::shared_ptr<BufferPool>`（管理生命周期） | 写权限（管理 Buffer） |
| **BufferPoolRegistry** | 统一管理所有 BufferPool<br>提供查询接口 | 持有 `shared_ptr`，提供查询接口 | ✅ 持有 `std::shared_ptr<BufferPool>`（统一管理） | 只读权限（查询接口）<br>读写权限（仅 ProductionLine 通过 friend） |
| **ProductionLine** | 管理 BufferPool 队列<br>协调生产流程 | `acquireFree()`<br>`submitFilled()`<br>管理队列状态 | ✅ 持有 `std::unique_ptr<BufferPool>`（使用期间） | 写权限（管理队列） |
| **用户** | 查询 BufferPool | 通过 Registry 查询 | ✅ 持有 `std::shared_ptr<const BufferPool>`（查询期间） | 只读 |
| **Display/Consumer** | 使用 BufferPool | `acquireFilled()`<br>`releaseFilled()`<br>`getBufferCount()` | ✅ 持有 `std::shared_ptr<const BufferPool>`（使用期间） | 只读 |

### 生命周期管理

- **Allocator 持有**：`std::shared_ptr<BufferPool>` 存储在 `managed_pools_` 中，管理 BufferPool 的生命周期
- **Registry 持有**：`std::shared_ptr<BufferPool>` 存储在 `pools_` 中，统一管理所有 BufferPool
- **ProductionLine 持有**：`std::unique_ptr<BufferPool>` 从 Worker 获取，管理 BufferPool 的使用
- **Worker 不持有**：创建后立即转换给 ProductionLine，不再持有
- **Display/Consumer 持有**：通过 Registry 获取 `shared_ptr<const BufferPool>`，只读访问

**生命周期保证**：
- Allocator 和 Registry 持有 `shared_ptr`，确保 BufferPool 在使用期间不被销毁
- ProductionLine 持有 `unique_ptr`，管理 BufferPool 的使用
- 当所有持有者都释放时，BufferPool 自动销毁并注销

### 权限控制

#### 只读接口（公开，任何人都可以调用）

```cpp
// BufferPoolRegistry 提供的只读接口
std::shared_ptr<const BufferPool> getPoolReadOnly(uint64_t id) const;
std::shared_ptr<const BufferPool> getPoolReadOnlyByName(const std::string& name) const;
std::vector<std::shared_ptr<const BufferPool>> getAllPoolsReadOnly() const;
std::vector<std::shared_ptr<const BufferPool>> getPoolsByCategoryReadOnly(const std::string& category) const;
std::vector<std::shared_ptr<const BufferPool>> getWorkerPoolsReadOnly() const;
std::shared_ptr<const BufferPool> getWorkerPoolReadOnly(const std::string& worker_name) const;
```

#### 读写接口（仅 ProductionLine 可以调用）

```cpp
// BufferPoolRegistry 提供的读写接口（通过 friend 限制）
class BufferPoolRegistry {
private:
    friend class VideoProductionLine;  // 权限控制
    
public:
    std::shared_ptr<BufferPool> getPoolForProductionLine(uint64_t id);
    std::shared_ptr<BufferPool> getPoolByNameForProductionLine(const std::string& name);
};
```

### 优势

- ✅ **Worker 职责单一**：创建后只负责填充 buffer，不管理 BufferPool
- ✅ **Allocator 持有**：管理 BufferPool 的生命周期，确保在使用期间不被销毁
- ✅ **ProductionLine 管理队列**：负责 BufferPool 的队列管理，有写权限
- ✅ **Registry 统一管理**：所有 BufferPool 都在 Registry 中，便于查询
- ✅ **用户只读访问**：通过 Registry 获取只读版本
- ✅ **生命周期安全**：使用 `shared_ptr` 管理，自动释放
- ✅ **权限控制**：编译期权限控制（通过 friend 类）
- ✅ **符合 C++ 最佳实践**：使用智能指针管理资源

## 📋 修改点总结

### 1. BufferAllocatorBase（基类）
- ✅ 添加 `std::vector<std::shared_ptr<BufferPool>> managed_pools_` 成员变量
- ✅ 添加 `mutable std::mutex managed_pools_mutex_` 互斥锁
- ✅ `allocatePoolWithBuffers()` 返回 `std::shared_ptr<BufferPool>`（而不是 `unique_ptr`）
- ✅ 添加 `<vector>` 头文件

### 2. 所有 Allocator 实现类
- ✅ `NormalAllocator::allocatePoolWithBuffers()`：返回 `shared_ptr` 并持有
- ✅ `AVFrameAllocator::allocatePoolWithBuffers()`：返回 `shared_ptr` 并持有
- ✅ `FramebufferAllocator::allocatePoolWithBuffers()`：返回 `shared_ptr` 并持有
- ✅ 所有 `destroyPool()` 方法：从 `managed_pools_` 中移除
- ✅ 添加 `<algorithm>` 头文件（用于 `std::find_if`）

### 3. BufferAllocatorFacade
- ✅ `allocatePoolWithBuffers()` 返回 `std::shared_ptr<BufferPool>`

### 4. BufferPool
- ✅ `CreateEmpty()` 返回 `std::shared_ptr<BufferPool>`
- ✅ 在 `CreateEmpty()` 中自动注册到 Registry（此时已有 `shared_ptr`）
- ✅ 添加 `setRegistryId()` 方法

### 5. BufferPoolRegistry
- ✅ `registerPool()` 接收 `std::shared_ptr<BufferPool>`（而不是 `BufferPool*`）
- ✅ `PoolInfo` 存储 `std::shared_ptr<BufferPool>`
- ✅ 添加只读接口（公开，任何人都可以调用）
- ✅ 添加读写接口（仅 `VideoProductionLine` 通过 friend 调用）
- ✅ 添加 `friend class VideoProductionLine;` 声明
- ✅ 所有查询接口返回只读版本

### 6. WorkerBase
- ✅ `buffer_pool_` 改为 `std::shared_ptr<BufferPool>`
- ✅ `getOutputBufferPool()` 从 `shared_ptr` 转换为 `unique_ptr`（Allocator 和 Registry 仍持有 `shared_ptr`）

### 7. Worker 实现类
- ✅ `FfmpegDecodeVideoFileWorker::getOutputBufferPool()`：使用基类实现
- ✅ `FfmpegDecodeRtspWorker::getOutputBufferPool()`：使用基类实现

## 🔄 数据流和调用关系

```
1. Worker::open()
   ↓
2. allocator_.allocatePoolWithBuffers(...)
   ↓
3. Allocator 创建 BufferPool（shared_ptr）
   ↓
4. Allocator 持有 shared_ptr（managed_pools_）
   ↓
5. BufferPool 自动注册到 Registry（Registry 持有 shared_ptr）
   ↓
6. Worker 临时持有 shared_ptr（buffer_pool_）
   ↓
7. ProductionLine::start()
   ↓
8. worker_->getOutputBufferPool()（转换为 unique_ptr）
   ↓
9. ProductionLine 持有 unique_ptr（worker_buffer_pool_）
   ↓
10. ProductionLine 管理队列（acquireFree/submitFilled）
   ↓
11. 用户/Display/Consumer 通过 Registry 获取 BufferPool（只读）
```

## 📝 使用示例

### 示例1：Worker 创建 BufferPool

```cpp
// Worker 在 open() 时创建 BufferPool
bool FfmpegDecodeVideoFileWorker::open(const char* path) {
    // 调用 Allocator 创建 BufferPool（返回 shared_ptr）
    buffer_pool_ = allocator_.allocatePoolWithBuffers(
        buffer_count,
        frame_size,
        "FfmpegDecodeVideoFileWorker_" + std::string(path),
        "Video"
    );
    
    // Allocator 持有 shared_ptr（managed_pools_）
    // Registry 持有 shared_ptr（pools_）
    // Worker 临时持有 shared_ptr（buffer_pool_）
    
    return buffer_pool_ != nullptr;
}
```

### 示例2：ProductionLine 获取 BufferPool

```cpp
// ProductionLine 从 Worker 获取 BufferPool
bool VideoProductionLine::start(const Config& config) {
    // Worker 打开文件（创建 BufferPool）
    worker_->open(...);
    
    // 从 Worker 获取 BufferPool（转换为 unique_ptr）
    worker_buffer_pool_ = worker_->getOutputBufferPool();
    
    // ProductionLine 持有 unique_ptr（worker_buffer_pool_）
    // Allocator 和 Registry 仍持有 shared_ptr
    
    working_buffer_pool_ = worker_buffer_pool_.get();
    return true;
}
```

### 示例3：用户通过 Registry 查询 BufferPool

```cpp
// 用户通过 Registry 查询 BufferPool（只读）
int main() {
    // 创建生产线
    VideoProductionLine producer;
    producer.start(config);
    
    // 方式1：通过 Registry 查询（只读）
    auto pool_readonly = BufferPoolRegistry::getInstance()
        .getPoolReadOnlyByName("FfmpegDecodeVideoFileWorker_video.mp4");
    
    // 方式2：通过 Registry 查询所有 Worker Pool（只读）
    auto worker_pools = BufferPoolRegistry::getInstance()
        .getWorkerPoolsReadOnly();
    
    // 方式3：通过 ProductionLine 获取（只读）
    auto pool = producer.getWorkingBufferPool();
    
    return 0;
}
```

### 示例4：ProductionLine 通过 Registry 获取 BufferPool（读写）

```cpp
// ProductionLine 通过 Registry 获取 BufferPool（读写，通过 friend 权限）
bool VideoProductionLine::start(const Config& config) {
    // 方式1：从 Worker 获取（保持现有逻辑）
    worker_buffer_pool_ = worker_->getOutputBufferPool();
    
    // 方式2：通过 Registry 获取（读写版本，仅 ProductionLine 可以调用）
    std::string pool_name = "WorkerPool_" + std::string(worker_->getWorkerType());
    worker_buffer_pool_ = BufferPoolRegistry::getInstance()
        .getPoolByNameForProductionLine(pool_name);
    
    return true;
}
```

## 🎯 设计原则总结

1. **单一职责原则**：
   - Worker：只负责创建和填充 Buffer
   - Allocator：负责创建和管理 BufferPool 的生命周期
   - ProductionLine：负责管理 BufferPool 的队列
   - Registry：负责统一管理和查询

2. **依赖倒置原则**：
   - 上层依赖接口和基类，不依赖具体实现
   - Registry 通过接口管理 BufferPool

3. **接口隔离原则**：
   - 只读接口和读写接口分离
   - 通过 friend 类实现权限控制

4. **生命周期管理**：
   - 使用 `shared_ptr` 管理共享资源
   - Allocator 和 Registry 持有 `shared_ptr`，确保生命周期安全
   - ProductionLine 持有 `unique_ptr`，管理使用

5. **权限控制**：
   - 编译期权限控制（通过 friend 类）
   - 只读接口公开，读写接口受限

---

**文档维护：** AI SDK Team  
**最后更新：** 2025-01-XX  
**方案版本：** v1.0（Allocator 持有 + Registry 统一管理 + ProductionLine 使用）
