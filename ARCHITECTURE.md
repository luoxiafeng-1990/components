# ProductionLine 综合架构设计文档

## 📋 目录

1. [架构概述](#架构概述)
2. [核心类职责](#核心类职责)
3. [类协作关系](#类协作关系)
4. [设计模式](#设计模式)
5. [门面模式与工厂模式详细分析](#门面模式与工厂模式详细分析)
6. [数据流](#数据流)
7. [核心类详解](#核心类详解)
8. [使用示例](#使用示例)
9. [最佳实践](#最佳实践)
10. [代码规范与风格指南](#代码规范与风格指南)
11. [API参考](#api参考)
12. [常见问题](#常见问题)

---

## 架构概述

### 核心理念

**ProductionLine（生产流水线）架构** 采用"生产流水线"和"工人"的类比，清晰地表达了数据流向和职责划分：

- **ProductionLine（生产流水线）**：负责从Worker获取原材料（BufferPool），进行生产（填充Buffer）
- **Worker（工人）**：负责从不同数据源获取数据，填充Buffer，提供原材料（BufferPool）给ProductionLine
- **BufferPool（原材料仓库）**：管理Buffer队列，提供线程安全的调度接口
- **Allocator（分配器）**：负责Buffer和BufferPool的创建和生命周期管理

### 架构层次（基于接口和基类的设计）

```
┌─────────────────────────────────────────────────────────┐
│                   应用层（Application）                    │
│              VideoProductionLine + BufferPool             │
└───────────────────────┬─────────────────────────────────┘
                        │ 使用接口
┌───────────────────────▼─────────────────────────────────┐
│                   门面层（Facade）                        │
│         BufferFillingWorkerFacade（门面，v2.1）          │
│    BufferAllocatorFacade（Allocator门面）                │
│    （直接定义方法，不继承接口）                            │
└───────────────────────┬─────────────────────────────────┘
                        │ 使用工厂创建
┌───────────────────────▼─────────────────────────────────┐
│                   工厂层（Factory）                        │
│         BufferFillingWorkerFactory（Worker工厂）          │
│         BufferAllocatorFactory（Allocator工厂）           │
│    （通过基类创建实现，不依赖具体类）                      │
└───────────────────────┬─────────────────────────────────┘
                        │ 返回基类指针
┌───────────────────────▼─────────────────────────────────┐
│                   接口层（Interface）                      │
│  IVideoFileNavigator（Worker导航接口）                    │
│  BufferAllocatorBase（Allocator接口，纯抽象基类）         │
│    （定义契约，所有实现必须遵循）                          │
└───────────────────────┬─────────────────────────────────┘
                        │ 继承
┌───────────────────────▼─────────────────────────────────┐
│                   基类层（Base）                          │
│              WorkerBase（Worker统一基类）                │
│    （继承 IVideoFileNavigator，定义 Buffer 填充功能）     │
└───────────────────────┬─────────────────────────────────┘
                        │ 继承
┌───────────────────────▼─────────────────────────────────┐
│                   实现层（Implementation）                  │
│  Worker实现类（继承WorkerBase，实现纯虚函数）              │
│  Allocator实现类（继承BufferAllocatorBase，实现接口方法）  │
│    （具体实现细节，对上层透明）                            │
└─────────────────────────────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│                   调度层（Scheduler）                       │
│              BufferPool（纯调度器）                        │
│    （通过接口与Allocator协作，不依赖具体实现）              │
└─────────────────────────────────────────────────────────┘
```

**设计原则**：
- ✅ **依赖倒置**：上层依赖接口和基类，不依赖具体实现
- ✅ **接口隔离**：接口定义清晰，职责单一
- ✅ **开闭原则**：对扩展开放，对修改关闭（新增实现无需修改接口）
- ✅ **单一职责**：每个层次职责明确，接口层定义契约，基类层提供公共功能，实现层提供具体逻辑

---

## 核心类职责

### 1. VideoProductionLine（生产流水线）

**职责：**
- ✅ **生产管理**：管理多个生产者线程，协调Buffer的获取、填充、提交流程
- ✅ **BufferPool获取**：从Worker获取BufferPool（原材料），Worker必须在实现`IVideoFileNavigator::open()`时自动创建BufferPool（通过调用Allocator）
- ✅ **线程协调**：使用原子变量管理帧索引，确保多线程安全
- ✅ **性能监控**：统计生产速度、错误率等

**不负责：**
- ❌ 视频解码/读取（由Worker负责）
- ❌ Buffer创建/销毁（由Allocator负责，Worker调用）
- ❌ Buffer队列管理（由BufferPool负责）
- ❌ BufferPool创建（由Worker负责，Worker调用Allocator创建）

### 2. BufferPool（缓冲区池）

**职责：**
- ✅ **队列调度**：管理空闲队列（free_queue）和填充队列（filled_queue）
- ✅ **线程安全**：提供线程安全的Buffer获取和提交接口
- ✅ **状态管理**：跟踪Buffer的状态（IDLE、LOCKED_BY_PRODUCER、READY_FOR_CONSUME、LOCKED_BY_CONSUMER）
- ✅ **注册管理**：自动注册到BufferPoolRegistry，支持全局查询和监控
- ✅ **创建权限控制**：通过 Passkey Idiom 限制创建权限，只有 Allocator 可以创建 BufferPool

**不负责：**
- ❌ Buffer创建/销毁（由Allocator负责）
- ❌ 数据填充（由Worker负责）
- ❌ 生产流程管理（由ProductionLine负责）

### 3. WorkerBase（Worker统一基类）- v2.0架构

**职责：**
- ✅ **定义Buffer填充功能**：通过纯虚函数定义契约 (`fillBuffer()`, `getWorkerType()`, `getOutputBufferPoolId()`)
- ✅ **继承文件导航接口**：继承`IVideoFileNavigator`接口，提供文件操作功能
- ✅ **BufferPool创建**：在实现`open()`时**必须**自动调用Allocator创建BufferPool
  - Worker内部持有`BufferAllocatorFacade`实例（通过构造函数参数指定类型）
  - Worker调用`allocator_facade_.allocatePoolWithBuffers()`创建BufferPool
  - Worker记录`buffer_pool_id_`（v2.0：Registry 独占持有 Pool）
  - 使用者从Registry通过 pool_id 获取临时访问
- ✅ **统一Allocator管理**：通过构造函数参数传递AllocatorType，父类统一管理

**v2.0架构变更：**
- ❌ 删除了独立的`IBufferFillingWorker`接口（已整合到 WorkerBase）
- ✅ WorkerBase直接定义Buffer填充功能的纯虚函数
- ✅ 简化架构，减少不必要的抽象层
- ✅ 所有Worker实现类只需继承WorkerBase一个基类
- ✅ 继承关系简化为：IVideoFileNavigator → WorkerBase → 具体实现类
- ✅ Registry中心化管理：Worker只记录pool_id，Registry独占持有BufferPool

**不负责：**
- ❌ Buffer创建/销毁（由Allocator负责，Worker只调用Allocator的方法）
- ❌ Buffer队列管理（由BufferPool负责）
- ❌ 生产流程管理（由ProductionLine负责）

**关键设计**：
- Worker在实现`open()`时**必须**创建BufferPool并记录pool_id
- Worker通过调用Allocator创建BufferPool，而不是直接创建
- Worker根据场景在构造函数中指定合适的AllocatorType（NORMAL、AVFRAME等）

### 4. IVideoFileNavigator（文件导航接口）

**职责**：
- ✅ **文件打开/关闭**：`open(path)` 和 `open(path, width, height, bits_per_pixel)`（两个重载），`close()`, `isOpen()`
- ✅ **文件导航**：`seek()`, `seekToBegin()`, `seekToEnd()`, `skip()`
- ✅ **文件状态查询**：`getTotalFrames()`, `getCurrentFrameIndex()`, `getFrameSize()`, `getFileSize()`, `getWidth()`, `getHeight()`, `getBytesPerPixel()`, `getPath()`, `hasMoreFrames()`, `isAtEnd()`

**继承关系（v2.0）**：
- `WorkerBase`继承`IVideoFileNavigator`接口
- Worker实现类继承`WorkerBase`基类：`class FfmpegWorker : public WorkerBase`
- 简化架构：继承链为 IVideoFileNavigator → WorkerBase → 具体实现类

**设计特点**：
- 职责清晰：文件操作功能独立为IVideoFileNavigator接口
- 可扩展：未来可以独立扩展文件操作功能
- 文档明确：通过接口名称明确表达职责

**注意**：
- Worker在实现`open()`时，需要同时处理文件打开逻辑和BufferPool创建逻辑
- 文件操作方法与Buffer填充操作分离，但都在WorkerBase中定义
- 所有Worker实现类（`FfmpegDecodeVideoFileWorker`, `MmapRawVideoFileWorker`, `FfmpegDecodeRtspWorker`, `IoUringRawVideoFileWorker`）都继承`WorkerBase`基类

### 5. BufferAllocator（分配器）

**职责：**
- ✅ **Buffer创建**：创建Buffer实例（调用子类的`createBuffer()`）
- ✅ **Buffer销毁**：销毁Buffer实例（调用子类的`deallocateBuffer()`）
- ✅ **BufferPool创建**：通过 Passkey Token 创建 BufferPool 实例（使用 `token()` 方法获取通行证）
- ✅ **Buffer注入**：将Buffer注入到BufferPool的队列中（通过友元关系访问BufferPool的私有方法）
- ✅ **Buffer移除**：从BufferPool移除Buffer（通过友元关系）

**不负责：**
- ❌ Buffer队列调度（由BufferPool负责）
- ❌ 数据填充（由Worker负责）
- ❌ 生产流程管理（由ProductionLine负责）

---

## 类协作关系

### 协作关系图（基于接口和基类）

```
┌─────────────────────────────────────────────────────────────────┐
│                    VideoProductionLine（应用层）                  │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  uint64_t working_buffer_pool_id_                        │  │
│  │  BufferPool* working_buffer_pool_ptr_                    │  │
│  │  std::shared_ptr<BufferFillingWorkerFacade> worker_      │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                                 │
│  协作关系（通过WorkerBase，v2.0）：                            │
│  1. 通过 WorkerBase::getOutputBufferPoolId() 获取Pool ID      │
│  2. 通过 WorkerBase::fillBuffer() 填充Buffer                  │
│  3. 通过 IVideoFileNavigator::open() 打开视频源               │
│  4. 通过 BufferPoolRegistry::getPool(pool_id) 获取Pool临时访问 │
└───────────────────────┬───────────────────────────────────────┘
                        │
                        │ 使用基类（不依赖具体实现）
                        │
        ┌───────────────┼───────────────┐
        │               │               │
┌───────▼──────┐ ┌──────▼──────┐ ┌──────▼──────┐
│ BufferPool   │ │ WorkerBase   │ │BufferAllocator│
│ (调度器)     │ │ (基类)       │ │Base(接口)    │
│              │ │              │ │              │
│ 通过接口协作 │ │ 定义方法     │ │ 定义接口     │
└──────────────┘ └─────────────┘ └─────────────┘
                        │               │
                        │ 继承           │ 继承
                        │               │
        ┌───────────────┼───────────────┼───────────────┐
        │               │               │               │
    Worker实现类    Worker实现类    Allocator实现类  Allocator实现类
    (具体实现)      (具体实现)      (具体实现)      (具体实现)
        │               │               │               │
        └───────────────┴───────────────┴───────────────┘
                        │
                        │ 通过Factory创建（返回基类指针）
                        │
        ┌───────────────▼───────────────┐
        │   Factory（工厂层）            │
        │   - BufferFillingWorkerFactory │
        │   - BufferAllocatorFactory      │
        │   （不依赖具体实现类）          │
        └───────────────────────────────┘
```

**关键设计点（v2.0）**：
- ✅ **依赖基类**：ProductionLine 依赖 `WorkerBase` 基类，通过其定义的纯虚函数调用功能
- ✅ **基类统一**：所有 Worker 实现通过 `WorkerBase` 基类统一类型，Factory 返回 `WorkerBase*`
- ✅ **接口定义**：`BufferAllocatorBase` 是纯抽象接口，定义所有 Allocator 必须实现的契约
- ✅ **工厂解耦**：Factory 通过基类创建实现，不依赖具体实现类
- ✅ **Registry 中心化**：BufferPool 由 Registry 独占持有，Worker 和 ProductionLine 只记录 pool_id

### 详细协作流程

#### 1. 初始化阶段（ProductionLine::start()）

```
1. ProductionLine::start(config)
   │
   ├─ 创建Worker（通过Factory）
   │   └─ BufferFillingWorkerFactory::create(worker_type)
   │
   ├─ 打开视频源（Worker在实现IVideoFileNavigator::open()时自动创建BufferPool）
   │   └─ worker_->open(...)  // 调用IVideoFileNavigator::open()
   │       │
   │       ├─ Worker必须创建BufferPool（通过调用Allocator）
   │       ├─ Worker创建Allocator实例（根据场景选择合适的Allocator）
   │       │   ├─ NormalAllocator（普通内存，用于Raw视频文件）
   │       │   ├─ AVFrameAllocator（FFmpeg解码，用于RTSP流和编码视频）
   │       │   └─ FramebufferAllocator（外部内存，用于Framebuffer显示）
   │       ├─ Worker调用 allocator->allocatePoolWithBuffers(...)
   │       │   │
   │       │   ├─ Allocator 通过 Passkey Token 创建空的 BufferPool
   │       │   │   └─ std::make_unique<BufferPool>(token(), name, category)
   │       │   │       ├─ token() 从 BufferAllocatorBase 基类获取通行证
   │       │   │       └─ 只有 Allocator 可以创建 PrivateToken
   │       │   │       └─ 返回 unique_ptr（转移所有权给Worker）
   │       │   │
   │       │   ├─ Allocator创建Buffer（调用子类的createBuffer）
   │       │   │   └─ NormalAllocator::createBuffer(id, size)
   │       │   │
   │       │   └─ Allocator注入Buffer到Pool（通过友元关系）
   │       │       └─ BufferPool::addBufferToQueue(buffer, FREE)
   │       │
   │       └─ Worker保存创建的BufferPool（内部成员）
   │
   ├─ 从Worker获取BufferPool（Worker必须返回非nullptr）
   │   └─ worker_buffer_pool_ = worker_->getOutputBufferPool()
   │       ├─ 如果返回nullptr → start()失败，报错："Worker failed to create BufferPool"
   │       └─ 返回非nullptr → 使用Worker的BufferPool
   │           └─ working_buffer_pool_ = worker_buffer_pool_.get()
   │
   └─ 启动生产者线程
       └─ producerThreadFunc(thread_id)
```

#### 2. 生产阶段（ProductionLine::producerThreadFunc()）

```
生产者线程循环：
   │
   ├─ 1. 从BufferPool获取空闲Buffer
   │   └─ buffer = working_buffer_pool_->acquireFree(true, timeout)
   │       │
   │       └─ BufferPool内部：
   │           ├─ 加锁（mutex_）
   │           ├─ 从free_queue取出Buffer
   │           ├─ 设置Buffer状态为LOCKED_BY_PRODUCER
   │           └─ 返回Buffer*
   │
   ├─ 2. 调用Worker填充Buffer
   │   └─ worker_->fillBuffer(frame_index, buffer)
   │       │
   │       └─ Worker内部：
   │           ├─ 从数据源读取/解码数据
   │           ├─ 填充到buffer->getVirtualAddress()
   │           └─ 返回成功/失败
   │
   ├─ 3. 提交填充后的Buffer
   │   └─ working_buffer_pool_->submitFilled(buffer)
   │       │
   │       └─ BufferPool内部：
   │           ├─ 加锁（mutex_）
   │           ├─ 设置Buffer状态为READY_FOR_CONSUME
   │           ├─ 添加到filled_queue
   │           └─ 通知消费者（filled_cv_.notify_one()）
   │
   └─ 4. 消费者从BufferPool获取填充后的Buffer
       └─ consumer->acquireFilled(true, timeout)
           │
           └─ BufferPool内部：
               ├─ 加锁（mutex_）
               ├─ 从filled_queue取出Buffer
               ├─ 设置Buffer状态为LOCKED_BY_CONSUMER
               └─ 返回Buffer*
```

#### 3. Worker扩展BufferPool（动态注入模式）

```
Worker内部解码循环（适用于RTSP流等）：
   │
   ├─ 1. FFmpeg解码获得AVFrame
   │   └─ avcodec_receive_frame(codec_ctx, frame)
   │
   ├─ 2. 调用Allocator注入Buffer
   │   └─ allocator->injectAVFrameToPool(frame, pool)
   │       │
   │       ├─ Allocator创建Buffer包装AVFrame
   │       │   └─ AVFrameAllocator::createBuffer(id, size)
   │       │
   │       ├─ Allocator注入Buffer到Pool（通过友元关系）
   │       │   └─ BufferPool::addBufferToQueue(buffer, FILLED)
   │       │
   │       └─ Allocator记录AVFrame和Buffer的映射
   │
   └─ 3. 消费者从BufferPool获取填充后的Buffer
       └─ pool->acquireFilled(true, timeout)
```

### 所有权关系

| 类 | 拥有的资源 | 所有权方式 | 说明 |
|---|-----------|-----------|------|
| **BufferPoolRegistry** | BufferPool | `std::shared_ptr<BufferPool>` | Registry 独占持有（引用计数=1），中心化管理 |
| **ProductionLine** | `working_buffer_pool_id_` | `uint64_t` | 只记录 pool_id，从 Registry 临时访问 |
| **ProductionLine** | `working_buffer_pool_ptr_` | `BufferPool*` | 缓存的临时访问指针（警告：Pool 销毁后失效） |
| **ProductionLine** | `worker_` | `std::shared_ptr<BufferFillingWorkerFacade>` | 多线程共享Worker门面 |
| **Worker** | `allocator_facade_`（内部） | `BufferAllocatorFacade` | Worker持有Allocator门面，用于创建BufferPool和Buffer |
| **Worker** | `buffer_pool_id_`（内部） | `uint64_t` | 只记录 pool_id，Registry 独占持有 Pool |
| **Allocator** | `Buffer`对象 | 通过`createBuffer()`创建 | Allocator负责Buffer的生命周期管理 |
| **Allocator** | BufferPool | ❌ **不持有** | Allocator创建BufferPool后注册到Registry，Registry独占持有 |
| **BufferPool** | `Buffer`对象 | 通过`managed_buffers_`集合管理 | BufferPool只管理Buffer的调度，不拥有Buffer |

### 关联方式

| 类 | 关联的资源 | 关联方式 | 说明 |
|---|-----------|---------|------|
| **BufferPoolRegistry** | BufferPool | `shared_ptr<BufferPool>` | Registry独占持有（引用计数=1），中心化管理 |
| **ProductionLine** | BufferPool | `uint64_t pool_id` | 只记录ID，通过Registry临时访问 |
| **ProductionLine** | Worker | `std::shared_ptr<BufferFillingWorkerFacade>` | 通过智能指针持有Worker门面 |
| **Worker** | BufferPool | `uint64_t pool_id` | 只记录ID，Registry独占持有Pool |
| **Worker** | Allocator | `BufferAllocatorFacade` | Worker内部持有Allocator门面，用于创建BufferPool和Buffer |
| **Allocator** | BufferPool | Friend关系 + 注册到Registry | Allocator是BufferPool的友元，创建后注册到Registry，Registry独占持有 |
| **BufferPool** | Buffer | `std::set<Buffer*>` | BufferPool通过集合管理所有Buffer，但不拥有Buffer的所有权 |

**核心设计原则（v2.0）**：
- ✅ **Registry 中心化**：Registry 独占持有 BufferPool（shared_ptr，引用计数=1）
- ✅ **ID 索引**：Worker 和 ProductionLine 只记录 pool_id，不持有所有权
- ✅ **临时访问**：通过 `BufferPoolRegistry::getInstance().getPool(pool_id)` 获取临时访问
- ✅ **Allocator ID 机制**：每个 Allocator 有唯一 ID，Registry 记录 Pool 归属关系
- ✅ **自动清理**：Allocator 析构时查询 Registry 获取所有 Pool，逐个清理
- ✅ **Worker 主动清理**：Worker 的 `close()` 调用 `destroyPool()` 主动清理资源

---

## 设计模式

### 1. 策略模式（Strategy Pattern）

**应用位置**：`WorkerBase` 基类及其实现类

**设计意图**：将填充Buffer的不同算法封装成独立的策略类，使它们可以互相替换。

**实现方式**：
- **策略基类**：`WorkerBase` 定义统一的填充Buffer接口（纯虚函数）
- **具体策略**：
  - `FfmpegDecodeVideoFileWorker`：FFmpeg解码策略
  - `MmapRawVideoFileWorker`：内存映射策略
  - `IoUringRawVideoFileWorker`：异步I/O策略
  - `FfmpegDecodeRtspWorker`：RTSP流解码策略

**优势**：
- 可扩展：新增Worker只需继承WorkerBase实现纯虚函数
- 可替换：不同Worker可以互相替换
- 解耦合：ProductionLine依赖WorkerBase基类，不依赖具体实现

### 2. 工厂模式（Factory Pattern）

**应用位置**：`BufferFillingWorkerFactory`、`BufferAllocatorBase`

**设计意图**：封装对象的创建逻辑，根据环境和配置创建合适的实例。

**实现方式**：
- **工厂类**：`BufferFillingWorkerFactory` 提供静态工厂方法
- **创建策略**（优先级从高到低）：
  1. 用户显式指定（`WorkerType`）
  2. 环境变量（`VIDEO_READER_TYPE`）
  3. 配置文件（`/etc/video_reader.conf`）
  4. 自动检测系统能力

**工厂模式类型**：
1. **工厂模式**：`BufferFillingWorkerFactory` - 创建Worker实现类
2. **抽象工厂模式**：`BufferAllocatorBase` - 创建Buffer和BufferPool，有3个具体实现：
   - `NormalAllocator` - 普通内存分配器
   - `FramebufferAllocator` - Framebuffer分配器
   - `AVFrameAllocator` - AVFrame分配器

**注意**：`BufferPool` 不再使用静态工厂方法 `CreateEmpty()`，改用 **Passkey Idiom** 控制创建权限。

### 3. 门面模式（Facade Pattern）

**应用位置**：`BufferFillingWorkerFacade`

**设计意图**：为复杂的Worker子系统提供统一的、简化的接口。

**实现方式**：
- **门面类**：`BufferFillingWorkerFacade` 封装底层Worker实现
- **隐藏复杂性**：
  - 自动选择Worker类型
  - 智能判断open参数（编码视频 vs Raw视频）
  - 统一错误处理
- **使用WorkerBase**：门面类持有 `std::unique_ptr<WorkerBase>`，无需dynamic_cast即可访问两个接口

**隐藏的子系统**：
- `FfmpegDecodeVideoFileWorker` - FFmpeg解码视频文件
- `MmapRawVideoFileWorker` - Mmap方式读取raw视频
- `IoUringRawVideoFileWorker` - IoUring方式读取raw视频
- `FfmpegDecodeRtspWorker` - FFmpeg解码RTSP流

### 4. 依赖注入（Dependency Injection）

**应用位置**：`VideoProductionLine` 和 `IBufferFillingWorker`

**设计意图**：通过构造函数或方法注入依赖，实现松耦合。

**实现方式**：
- **Worker提供BufferPool**（智能指针方案）：
  ```cpp
  // Worker在实现IVideoFileNavigator::open()时自动调用Allocator创建BufferPool（必须）
  std::unique_ptr<BufferPool> worker_buffer_pool_ = worker_->getOutputBufferPool();
  // ProductionLine持有Worker创建的BufferPool的所有权
  // 如果Worker返回nullptr，start()会失败
  ```

### 5. 生产者-消费者模式（Producer-Consumer Pattern）

**应用位置**：`VideoProductionLine` 和 `BufferPool`

**设计意图**：通过BufferPool作为中间缓冲区，解耦生产者和消费者。

**实现方式**：
- **生产者**：`VideoProductionLine` 的生产者线程
- **缓冲区**：`BufferPool` 管理空闲队列和填充队列
- **消费者**：外部应用从BufferPool获取填充后的Buffer

### 6. 友元模式（Friend Pattern）

**应用位置**：`BufferAllocator` 和 `BufferPool`

**设计意图**：允许Allocator访问BufferPool的私有方法，同时保持封装性。

**实现方式**：
- `BufferAllocator` 是 `BufferPool` 的友元类
- Allocator可以访问BufferPool的私有方法：
  - `addBufferToQueue()`：添加Buffer到队列
  - `removeBufferFromPool()`：从Pool移除Buffer

### 7. Passkey Idiom（通行证模式）

**应用位置**：`BufferPool` 和 `BufferAllocatorBase`

**设计意图**：限制类的实例化权限，只有特定的类（Allocator）可以创建 BufferPool 实例，提供比 friend 更精细的访问控制。

**实现方式**：
- `BufferPool` 有一个嵌套类 `PrivateToken`，其构造函数是 `private`
- 只有 `BufferAllocatorBase` 是 `PrivateToken` 的 `friend`，可以创建 Token
- `BufferAllocatorBase` 提供 `protected static token()` 方法供子类获取 Token
- 子类通过 `token()` 获取 PrivateToken，然后调用 BufferPool 构造函数

**代码示例**：
```cpp
// BufferPool.hpp
class BufferPool {
public:
    // 嵌套的 PrivateToken 类
    class PrivateToken {
    private:
        PrivateToken() = default;
        // 只有 BufferAllocatorBase 可以创建 Token
        friend class BufferAllocatorBase;
    };
    
    // 构造函数需要 Token（虽然是 public，但外部无法创建 Token）
    BufferPool(
        PrivateToken token,
        const std::string& name,
        const std::string& category
    );
};

// BufferAllocatorBase.hpp
class BufferAllocatorBase {
protected:
    // 提供 Token 给子类使用
    static BufferPool::PrivateToken token() {
        return BufferPool::PrivateToken();
    }
};

// 子类使用示例（NormalAllocator.cpp）
auto pool = std::make_unique<BufferPool>(
    token(),    // 从基类获取通行证
    name,
    category
);
// 注册到Registry（使用weak_ptr，不持有所有权）
std::shared_ptr<BufferPool> temp_shared = std::shared_ptr<BufferPool>(
    pool.get(), [](BufferPool*) {}  // 空删除器
);
uint64_t id = BufferPoolRegistry::getInstance().registerPoolWeak(temp_shared);
pool->setRegistryId(id);
temp_shared.reset();  // 释放临时shared_ptr
// 返回unique_ptr（转移所有权）
return pool;
```

**优势**：
- ✅ **精细控制**：比 friend 更精细，只授权创建权限，不授权访问所有私有成员
- ✅ **类型安全**：编译期类型检查，Token 无法伪造
- ✅ **代码简洁**：不需要额外的 bridge 函数或工厂方法
- ✅ **语义清晰**：通过 Token 明确表达"持有通行证才能创建"的语义
- ✅ **易于维护**：所有创建逻辑在子类中，无需在基类中实现

**与其他方案对比**：
- **vs. Public 静态工厂方法**：Passkey 更严格，外部无法创建
- **vs. Private 构造 + Friend**：Passkey 更灵活，子类可以直接使用
- **vs. 基类 Bridge 函数**：Passkey 更简洁，无需额外函数

---

## 门面模式与工厂模式详细分析

### 概述

本节详细分析 `packages/components` 目录中门面模式（Facade Pattern）和工厂模式（Factory Pattern）的使用，以及它们之间的关系。

### 门面类识别

#### ✅ BufferFillingWorkerFacade（门面类）

**文件位置**:
- 头文件: `include/productionline/worker/facade/BufferFillingWorkerFacade.hpp`
- 源文件: `source/productionline/worker/facade/BufferFillingWorkerFacade.cpp`

**设计模式**: 门面模式（Facade Pattern）

**设计变更（v2.1）**：
- ❌ 删除对 `IBufferFillingWorker` 和 `IVideoFileNavigator` 接口的继承
- ✅ 不继承任何接口或基类，直接定义所有方法
- ✅ 通过组合模式持有 `WorkerBase` 指针，所有方法转发

**职责**:
- 为用户提供统一、简单的Buffer填充操作接口
- 隐藏底层多种实现（mmap、io_uring、FFmpeg等）的复杂性
- 自动选择最优的Worker实现

**特点**:
- 统一的API接口，简化使用
- 底层实现可以透明切换
- 支持自动和手动选择Worker类型
- 使用组合模式（持有 WorkerBase 指针），所有方法转发给内部实现

**门面模式体现（v2.1）**:
```cpp
class BufferFillingWorkerFacade {
    // v2.1: 不继承任何接口
private:
    std::unique_ptr<WorkerBase> worker_base_uptr_;  // 持有具体实现（统一基类）
    BufferFillingWorkerFactory::WorkerType preferred_type_;
    
public:
    // 直接定义所有方法，不使用 override 关键字
    bool open(const char* path);
    bool open(const char* path, int width, int height, int bits_per_pixel);
    bool fillBuffer(int frame_index, Buffer* buffer);
    uint64_t getOutputBufferPoolId();
    // ... 其他方法
    // 所有方法转发给 worker_base_uptr_
};
```

### 工厂模式识别

#### ✅ BufferFillingWorkerFactory（工厂类）

**文件位置**:
- 头文件: `include/productionline/worker/factory/BufferFillingWorkerFactory.hpp`
- 源文件: `source/productionline/worker/factory/BufferFillingWorkerFactory.cpp`

**设计模式**: 工厂模式（Factory Pattern）

**职责**:
- 根据环境和配置创建合适的Worker实现
- 封装Worker创建逻辑
- 支持自动检测和手动指定两种模式
- 返回WorkerBase基类指针，统一类型系统

**工厂方法**:
```cpp
class BufferFillingWorkerFactory {
public:
    enum class WorkerType {
        AUTO,              // 自动检测
        MMAP_RAW,          // MmapRawVideoFileWorker
        IOURING_RAW,       // IoUringRawVideoFileWorker
        FFMPEG_RTSP,       // FfmpegDecodeRtspWorker
        FFMPEG_VIDEO_FILE  // FfmpegDecodeVideoFileWorker
    };
    
    // 工厂方法（返回WorkerBase基类）
    static std::unique_ptr<WorkerBase> create(WorkerType type = WorkerType::AUTO);
    static std::unique_ptr<WorkerBase> createByName(const char* name);
    
private:
    static std::unique_ptr<WorkerBase> createByType(WorkerType type);
    static std::unique_ptr<WorkerBase> autoDetect();
};
```

**创建的产品**:
- `MmapRawVideoFileWorker`
- `IoUringRawVideoFileWorker`
- `FfmpegDecodeRtspWorker`
- `FfmpegDecodeVideoFileWorker`

#### ✅ BufferAllocatorBase（Allocator接口，纯抽象基类）

**文件位置**:
- 接口: `include/buffer/allocator/base/BufferAllocatorBase.hpp`
- 实现类: `include/buffer/allocator/implementation/`（NormalAllocator, AVFrameAllocator, FramebufferAllocator）

**设计模式**: 抽象工厂模式（Abstract Factory Pattern）

**架构角色**: 纯抽象接口类（所有方法都是纯虚函数）

**职责**:
- 定义所有 Allocator 必须实现的接口契约
- 创建 Buffer 和 BufferPool
- 管理 Buffer 生命周期

**接口定义**（纯虚函数，子类必须实现）:
```cpp
class BufferAllocatorBase {
public:
    virtual ~BufferAllocatorBase() = default;
    
    // 纯虚函数接口（子类必须实现）
    virtual std::unique_ptr<BufferPool> allocatePoolWithBuffers(
        int count, size_t size,
        const std::string& name,
        const std::string& category = ""
    ) = 0;
    
    virtual Buffer* injectBufferToPool(
        size_t size,
        BufferPool* pool,
        QueueType queue = QueueType::FREE
    ) = 0;
    
    virtual bool removeBufferFromPool(Buffer* buffer, BufferPool* pool) = 0;
    
    virtual bool destroyPool(BufferPool* pool) = 0;
    
protected:
    // 子类必须实现的核心方法
    virtual Buffer* createBuffer(uint32_t id, size_t size) = 0;
    virtual void deallocateBuffer(Buffer* buffer) = 0;
};
```

**设计特点**:
- ✅ **纯抽象接口**：所有方法都是纯虚函数（`= 0`），只有头文件，无实现文件
- ✅ **接口契约**：定义所有 Allocator 必须实现的完整接口
- ✅ **依赖倒置**：上层代码依赖 `BufferAllocatorBase` 接口，不依赖具体实现
- ✅ **实现透明**：具体实现类（NormalAllocator、AVFrameAllocator、FramebufferAllocator）对上层透明

#### ✅ BufferAllocatorFactory（Allocator工厂）

**文件位置**:
- 工厂: `include/buffer/allocator/factory/BufferAllocatorFactory.hpp`
- 源文件: `source/buffer/allocator/factory/BufferAllocatorFactory.cpp`

**设计模式**: 工厂模式（Factory Pattern）

**职责**:
- 根据类型创建合适的 Allocator 实现
- 封装 Allocator 创建逻辑
- 返回 `BufferAllocatorBase*` 接口指针，不依赖具体实现类

**工厂方法**:
```cpp
class BufferAllocatorFactory {
public:
    enum class AllocatorType {
        AUTO,           // 自动选择（默认使用 NormalAllocator）
        NORMAL,         // NormalAllocator
        AVFRAME,        // AVFrameAllocator
        FRAMEBUFFER     // FramebufferAllocator
    };
    
    // 工厂方法（返回接口指针）
    static std::unique_ptr<BufferAllocatorBase> create(
        AllocatorType type = AllocatorType::AUTO,
        BufferMemoryAllocatorType mem_type = BufferMemoryAllocatorType::NORMAL_MALLOC,
        size_t alignment = 64
    );
};
```

**设计特点**:
- ✅ **接口返回**：返回 `BufferAllocatorBase*` 接口指针，不返回具体实现类
- ✅ **解耦合**：Factory 不依赖具体实现类，只依赖接口
- ✅ **统一创建**：所有 Allocator 类型通过统一接口创建

#### ✅ BufferAllocatorFacade（Allocator门面）

**文件位置**:
- 门面: `include/buffer/allocator/facade/BufferAllocatorFacade.hpp`
- 源文件: `source/buffer/allocator/facade/BufferAllocatorFacade.cpp`

**设计模式**: 门面模式（Facade Pattern）

**职责**:
- 为用户提供统一、简单的 Buffer 分配接口
- 隐藏底层多种 Allocator 实现的复杂性
- 内部使用 Factory 创建 Allocator，对外提供统一接口

**设计特点**:
- ✅ **统一接口**：提供与 `BufferAllocatorBase` 一致的接口
- ✅ **内部使用 Factory**：构造函数内部通过 `BufferAllocatorFactory` 创建底层 Allocator
- ✅ **隐藏复杂性**：用户无需了解 Factory 和具体实现类

### 门面类使用工厂模式的关系

#### 🔗 BufferFillingWorkerFacade（门面）→ BufferFillingWorkerFactory（工厂）

**关系类型**: 门面类内部使用工厂模式创建具体实现

**代码证据**:
```cpp
// BufferFillingWorkerFacade.cpp
BufferFillingWorkerFacade::BufferFillingWorkerFacade(BufferFillingWorkerFactory::WorkerType type)
    : preferred_type_(type)
{
    if (!worker_) {
        // 🎯 门面类使用工厂创建具体实现（返回WorkerBase）
        worker_ = BufferFillingWorkerFactory::create(preferred_type_);
        // 无需dynamic_cast，直接使用worker_访问两个接口
    }
}

bool BufferFillingWorkerFacade::open(const char* path, int width, int height, int bits_per_pixel) {
    // 创建 worker（如果还没创建）
    if (!worker_) {
        // 🎯 门面类使用工厂创建具体实现（返回WorkerBase）
        worker_ = BufferFillingWorkerFactory::create(preferred_type_);
    }
    // 直接通过 worker_ 调用两个接口的方法
    return worker_->open(path, width, height, bits_per_pixel);
    // 或 worker_->open(path);  // 单参数重载
}
```

**设计优势**:
1. **解耦合**: 门面类不直接依赖具体实现类，只依赖工厂和接口
2. **可扩展**: 新增Worker实现只需修改工厂，门面类无需修改
3. **灵活性**: 支持自动检测和手动指定两种创建方式
4. **统一接口**: 门面类提供统一的API，隐藏底层实现的差异

### 可视化图表

#### 📊 完整架构关系图（基于接口和基类）

```mermaid
graph TB
    subgraph "应用层 Application"
        VPL[VideoProductionLine<br/>使用基类]
    end
    
    subgraph "门面层 Facade"
        BFW[BufferFillingWorkerFacade<br/>v2.1: 不继承接口]
        BAF[BufferAllocatorFacade<br/>封装接口]
    end
    
    subgraph "工厂层 Factory"
        BFWFactory[BufferFillingWorkerFactory<br/>返回基类指针]
        BAFactory[BufferAllocatorFactory<br/>返回接口指针]
    end
    
    subgraph "接口层 Interface"
        IVFN[IVideoFileNavigator<br/>Worker导航接口]
        BAB[BufferAllocatorBase<br/>Allocator接口<br/>纯抽象基类]
    end
    
    subgraph "基类层 Base"
        WB[WorkerBase<br/>统一基类<br/>继承IVideoFileNavigator<br/>定义Buffer填充方法]
    end
    
    subgraph "实现层 Implementation"
        WorkerImpl[Worker实现类<br/>继承WorkerBase]
        AllocatorImpl[Allocator实现类<br/>继承BufferAllocatorBase]
    end
    
    VPL -->|使用| BFW
    BFW -->|持有基类| WB
    BFW -->|使用工厂| BFWFactory
    BFWFactory -->|返回基类| WB
    WB -->|继承| IVFN
    WB -->|被继承| WorkerImpl
    
    BAF -->|封装接口| BAB
    BAF -->|使用工厂| BAFactory
    BAFactory -->|返回接口| BAB
    BAB -->|被继承| AllocatorImpl
    
    style IVFN fill:#99ff99,stroke:#333,stroke-width:3px
    style BAB fill:#99ff99,stroke:#333,stroke-width:3px
    style WB fill:#ffcc99,stroke:#333,stroke-width:3px
    style BFW fill:#ff9999,stroke:#333,stroke-width:2px
    style BAF fill:#ff9999,stroke:#333,stroke-width:2px
    style BFWFactory fill:#99ccff,stroke:#333,stroke-width:2px
    style BAFactory fill:#99ccff,stroke:#333,stroke-width:2px
```

#### 🏭 工厂模式详细关系图（基于接口和基类）

```mermaid
classDiagram
    class BufferFillingWorkerFactory {
        <<factory>>
        +create(WorkerType) WorkerBase*
        +createByName(string) WorkerBase*
        +autoDetect() WorkerBase*
        -createByType(WorkerType) WorkerBase*
    }
    
    class BufferAllocatorFactory {
        <<factory>>
        +create(AllocatorType) BufferAllocatorBase*
        +createByName(string) BufferAllocatorBase*
        -createByType(AllocatorType) BufferAllocatorBase*
    }
    
    class IVideoFileNavigator {
        <<interface>>
        +open(string) bool
        +open(string, int, int, int) bool
        +close() void
        +seek(int) bool
    }
    
    class BufferAllocatorBase {
        <<interface>>
        +allocatePoolWithBuffers(...) uint64_t
        +injectBufferToPool(...) Buffer*
        +removeBufferFromPool(...) bool
        +destroyPool(...) bool
        #createBuffer(...) Buffer*
        #deallocateBuffer(...) void
    }
    
    class WorkerBase {
        <<abstract base>>
        +fillBuffer(int, Buffer*) bool
        +getOutputBufferPoolId() uint64_t
        +open(string) bool
        #allocator_ BufferAllocatorFacade
        #buffer_pool_id_ uint64_t
    }
    
    class WorkerImplementation {
        <<implementation>>
        Worker实现类继承WorkerBase
        实现所有纯虚函数
    }
    
    class AllocatorImplementation {
        <<implementation>>
        Allocator实现类继承BufferAllocatorBase
        实现所有接口方法
    }
    
    BufferFillingWorkerFactory ..> WorkerBase : creates
    BufferAllocatorFactory ..> BufferAllocatorBase : creates
    IVideoFileNavigator <|.. WorkerBase : inherits
    WorkerBase <|.. WorkerImplementation : inherits
    BufferAllocatorBase <|.. AllocatorImplementation : inherits
```

#### 🎭 门面模式详细关系图（v2.1架构）

```mermaid
classDiagram
    class BufferFillingWorkerFacade {
        <<facade>>
        -worker_base_uptr_ WorkerBase*
        -preferred_type_ WorkerType
        +open(string) bool
        +fillBuffer(int, Buffer*) bool
        +getOutputBufferPoolId() uint64_t
        +所有方法（不使用override）
    }
    
    class BufferAllocatorFacade {
        <<facade>>
        -allocator_ BufferAllocatorBase*
        +allocatePoolWithBuffers(...) uint64_t
        +injectBufferToPool(...) Buffer*
        +所有接口方法...
    }
    
    class IVideoFileNavigator {
        <<interface>>
        +open(string) bool
        +open(string, int, int, int) bool
        +close() void
        +seek(int) bool
    }
    
    class BufferAllocatorBase {
        <<interface>>
        +allocatePoolWithBuffers(...) uint64_t
        +injectBufferToPool(...) Buffer*
        +removeBufferFromPool(...) bool
        +destroyPool(...) bool
    }
    
    class WorkerBase {
        <<abstract base>>
        +fillBuffer(int, Buffer*) bool
        +getOutputBufferPoolId() uint64_t
        +所有方法...
    }
    
    class BufferFillingWorkerFactory {
        <<factory>>
        +create(WorkerType) WorkerBase*
    }
    
    class BufferAllocatorFactory {
        <<factory>>
        +create(AllocatorType) BufferAllocatorBase*
    }
    
    BufferFillingWorkerFacade --> WorkerBase : holds
    BufferFillingWorkerFacade --> BufferFillingWorkerFactory : uses
    
    BufferAllocatorFacade --> BufferAllocatorBase : holds
    BufferAllocatorFacade --> BufferAllocatorFactory : uses
    
    BufferFillingWorkerFactory ..> WorkerBase : creates
    BufferAllocatorFactory ..> BufferAllocatorBase : creates
    
    IVideoFileNavigator <|.. WorkerBase : inherits
```

**v2.1 架构变更说明**：
- ❌ `BufferFillingWorkerFacade` 不再继承任何接口
- ✅ 通过组合模式持有 `WorkerBase` 指针，所有方法转发
- ✅ 简化架构，减少继承层次
- ✅ 保持 API 一致性，不影响使用者

#### 🏗️ Allocator架构关系图（基于接口）

```mermaid
classDiagram
    class BufferAllocatorBase {
        <<interface>>
        +allocatePoolWithBuffers(...) BufferPool*
        +injectBufferToPool(...) Buffer*
        +removeBufferFromPool(...) bool
        +destroyPool(...) bool
        #createBuffer(...) Buffer*
        #deallocateBuffer(...) void
    }
    
    class BufferAllocatorFactory {
        <<factory>>
        +create(AllocatorType) BufferAllocatorBase*
        +createByName(string) BufferAllocatorBase*
    }
    
    class BufferAllocatorFacade {
        <<facade>>
        -allocator_ BufferAllocatorBase*
        +allocatePoolWithBuffers(...) BufferPool*
        +injectBufferToPool(...) Buffer*
        +所有接口方法...
    }
    
    class AllocatorImplementation {
        <<implementation>>
        NormalAllocator
        AVFrameAllocator
        FramebufferAllocator
        继承BufferAllocatorBase
        实现所有接口方法
    }
    
    class BufferPool {
        <<friend>>
        +CreateEmpty(string, string) BufferPool*
        +acquireFree(bool, int) Buffer*
        +submitFilled(Buffer*) void
    }
    
    BufferAllocatorFacade --> BufferAllocatorBase : holds
    BufferAllocatorFacade --> BufferAllocatorFactory : uses
    BufferAllocatorFactory ..> BufferAllocatorBase : creates
    BufferAllocatorBase <|.. AllocatorImplementation : inherits
    BufferAllocatorBase ..> BufferPool : creates (friend)
```

#### 📁 完整文件依赖关系图（v2.1架构）

```mermaid
graph TD
    subgraph "productionline/worker/"
        VPL[VideoProductionLine.hpp<br/>应用层]
        BFW[facade/BufferFillingWorkerFacade.hpp<br/>🎭门面 v2.1]
        BFWFactory[factory/BufferFillingWorkerFactory.hpp<br/>🏭工厂]
        IVFN[interface/IVideoFileNavigator.hpp<br/>📋接口]
        WB[base/WorkerBase.hpp<br/>🔷基类]
        WorkerImpl[implementation/*.hpp<br/>实现类]
    end
    
    subgraph "buffer/allocator/"
        BAF[facade/BufferAllocatorFacade.hpp<br/>🎭门面]
        BAFactory[factory/BufferAllocatorFactory.hpp<br/>🏭工厂]
        BAB[base/BufferAllocatorBase.hpp<br/>📋接口]
        AllocatorImpl[implementation/*.hpp<br/>实现类]
    end
    
    subgraph "buffer/"
        BP[BufferPool.hpp<br/>调度器]
        BPR[BufferPoolRegistry.hpp<br/>Registry中心化]
        B[Buffer.hpp]
    end
    
    VPL -->|使用| BFW
    VPL --> BP
    VPL --> BPR
    BFW -->|持有| WB
    BFW -->|使用工厂| BFWFactory
    BFWFactory -->|返回基类| WB
    WB -->|继承| IVFN
    WB -->|被继承| WorkerImpl
    WorkerImpl -->|继承| WB
    
    BAF -->|封装| BAB
    BAF -->|使用工厂| BAFactory
    BAFactory -->|返回接口| BAB
    BAB -->|被继承| AllocatorImpl
    AllocatorImpl -->|继承| BAB
    
    BAB -->|注册到| BPR
    BPR -->|独占持有| BP
    BP --> B
    
    style IVFN fill:#99ff99,stroke:#333,stroke-width:3px
    style BAB fill:#99ff99,stroke:#333,stroke-width:3px
    style WB fill:#ffcc99,stroke:#333,stroke-width:3px
    style BFW fill:#ff9999,stroke:#333,stroke-width:2px
    style BAF fill:#ff9999,stroke:#333,stroke-width:2px
    style BFWFactory fill:#99ccff,stroke:#333,stroke-width:2px
    style BAFactory fill:#99ccff,stroke:#333,stroke-width:2px
    style BPR fill:#ffff99,stroke:#333,stroke-width:3px
```

#### 🔄 数据流和调用关系图

```mermaid
sequenceDiagram
    participant Client as VideoProductionLine
    participant Facade as BufferFillingWorkerFacade<br/>(门面)
    participant Factory as BufferFillingWorkerFactory<br/>(工厂)
    participant Worker as WorkerBase<br/>(基类/具体实现)
    participant BufferPool as BufferPool
    
    Client->>Facade: open(path, width, height, bpp)
    Facade->>Factory: create(WorkerType)
    Factory->>Worker: new MmapRawVideoFileWorker()
    Factory-->>Facade: worker instance
    Facade->>Worker: open(path, width, height, bpp)
    Worker->>BufferPool: 创建或获取 BufferPool
    Worker-->>Facade: success
    
    Client->>Facade: fillBuffer(frame_index, buffer)
    Facade->>Worker: fillBuffer(frame_index, buffer)
    Worker-->>Facade: success
    Facade-->>Client: success
    
    Note over Facade,Factory: 门面类使用工厂创建具体实现<br/>隐藏底层复杂性
```

### 设计模式统计表（v2.1架构）

| 设计模式 | 类/方法 | 文件位置 | 架构角色 | 返回类型 |
|---------|---------|---------|---------|---------|
| **门面模式（v2.1）** | BufferFillingWorkerFacade | `productionline/worker/facade/` | 门面层（不继承接口） | 直接定义方法 |
| **门面模式** | BufferAllocatorFacade | `buffer/allocator/facade/` | 门面层 | 封装接口 |
| **工厂模式** | BufferFillingWorkerFactory | `productionline/worker/factory/` | 工厂层 | 返回 `WorkerBase*` |
| **工厂模式** | BufferAllocatorFactory | `buffer/allocator/factory/` | 工厂层 | 返回 `BufferAllocatorBase*` |
| **接口层** | IVideoFileNavigator | `productionline/worker/interface/` | 接口层 | 定义契约 |
| **接口层** | BufferAllocatorBase | `buffer/allocator/base/` | 接口层（纯抽象） | 定义契约 |
| **基类层** | WorkerBase | `productionline/worker/base/` | 基类层 | 统一基类 |
| **Passkey Idiom** | BufferPool::PrivateToken | `buffer/BufferPool.hpp` | 通行证模式 | 限制 BufferPool 创建权限 |

**关键设计（v2.1）**：
- ✅ **接口定义契约**：`IVideoFileNavigator` 定义文件操作接口
- ✅ **基类统一类型**：`WorkerBase` 统一所有 Worker 实现类的类型，定义 Buffer 填充方法
- ✅ **工厂返回基类**：Factory 返回基类指针，不返回具体实现类
- ✅ **实现类透明**：具体实现类对上层透明，通过基类访问
- ✅ **Passkey 控制**：通过 PrivateToken 限制 BufferPool 创建权限，只有 Allocator 可以创建
- ✅ **门面不继承**：`BufferFillingWorkerFacade` 不继承接口，直接定义方法并转发

### 关键关系总结（基于接口和基类）

#### Worker架构关系

```
应用层（VideoProductionLine）
    ↓ 使用接口
门面层（BufferFillingWorkerFacade）
    ↓ 实现接口 + 持有基类
接口层（IBufferFillingWorker + IVideoFileNavigator）
    ↓ 定义契约
基类层（WorkerBase）
    ↓ 继承接口 + 提供公共功能
实现层（Worker实现类）
    ↓ 通过工厂创建
工厂层（BufferFillingWorkerFactory）
    ↓ 返回基类指针
基类层（WorkerBase）
```

#### Allocator架构关系

```
应用层（Worker）
    ↓ 使用门面
门面层（BufferAllocatorFacade）
    ↓ 封装接口 + 使用工厂
接口层（BufferAllocatorBase）
    ↓ 定义契约（纯抽象）
实现层（Allocator实现类）
    ↓ 通过工厂创建
工厂层（BufferAllocatorFactory）
    ↓ 返回接口指针
接口层（BufferAllocatorBase）
```

#### 设计模式组合优势

1. ✅ **依赖倒置**：上层依赖接口和基类，不依赖具体实现
2. ✅ **接口隔离**：接口定义清晰，职责单一
3. ✅ **开闭原则**：对扩展开放，对修改关闭（新增实现无需修改接口）
4. ✅ **统一接口**：通过接口和基类提供统一的API
5. ✅ **实现透明**：具体实现类对上层完全透明

---

## 数据流

### 整体数据流

```
视频源（RTSP/RAW/MP4）
    ↓
Worker（解码/读取）
    ↓
填充Buffer
    ↓
BufferPool（管理队列）
    ↓
ProductionLine（生产管理）
    ↓
消费者（显示/处理）
```

### 详细数据流（两种模式）

#### Worker填充Buffer流程

**所有Worker统一流程**：
```
1. ProductionLine::producerThreadFunc()
   ↓
2. buffer_pool_ptr_->acquireFree()  // 从BufferPool获取空闲Buffer
   │   （BufferPool由Worker在open()时自动创建）
   ↓
3. worker_->fillBuffer(frame_index, buffer)  // Worker填充Buffer
   │   ├── MmapRawVideoFileWorker: 从mmap区域memcpy到buffer->data()
   │   ├── IoUringRawVideoFileWorker: 异步读取到buffer->data()
   │   ├── FfmpegDecodeVideoFileWorker: 解码后memcpy到buffer->data()
   │   └── FfmpegDecodeRtspWorker: 解码后填充buffer元数据
   ↓
4. buffer_pool_ptr_->submitFilled(buffer)  // 提交填充后的Buffer
   ↓
5. 消费者从BufferPool获取填充后的Buffer
```

**注意**：
- 所有Worker都必须自己创建BufferPool（通过调用Allocator）
- Worker在实现`IVideoFileNavigator::open()`时自动创建BufferPool
- ProductionLine通过`getOutputBufferPool()`获取Worker创建的BufferPool

### BufferPool工作流程

```
空闲队列（Free Queue）
    ↓ acquireFree()
生产者线程获取Buffer
    ↓ fillBuffer()
填充数据
    ↓ submitFilled()
填充队列（Filled Queue）
    ↓ acquireFilled()
消费者获取Buffer
    ↓ releaseFilled()
空闲队列（Free Queue）
```

---

## 核心类详解

### 1. VideoProductionLine（生产流水线）

**职责**：
- 从Worker获取BufferPool（原材料）
- 管理多个生产者线程
- 协调Buffer的获取、填充、提交流程
- 性能监控和统计

**关键成员变量**：
- `std::unique_ptr<BufferPool> worker_buffer_pool_`：Worker创建的BufferPool（Worker通过调用Allocator创建，持有所有权）
**关键成员变量**：
- `uint64_t working_buffer_pool_id_`：Worker创建的BufferPool ID（v2.0：Registry独占持有）
- `BufferPool* working_buffer_pool_ptr_`：实际工作的BufferPool指针（缓存的临时访问）
- `std::shared_ptr<BufferFillingWorkerFacade> worker_facade_sptr_`：Worker门面（多线程共享）
- `std::vector<std::thread> threads_`：生产者线程池
- `std::atomic<int> next_frame_index_`：下一个要读取的帧索引（原子递增）

**核心方法**：
- `start(config)`：启动生产流水线
  1. 创建Worker（通过Factory）
  2. 打开视频源（调用`IVideoFileNavigator::open()`，Worker在实现时**必须**自动创建BufferPool，通过调用Allocator）
  3. 从Worker获取BufferPool ID（通过`WorkerBase::getOutputBufferPoolId()`，v2.0返回uint64_t）
  4. 验证Worker是否创建了BufferPool（如果返回0，start()失败）
  5. 从Registry获取临时访问（`BufferPoolRegistry::getInstance().getPool(pool_id)`）
  6. 启动生产者线程
- `producerThreadFunc(thread_id)`：生产者线程函数
  1. 原子获取帧索引
  2. 从BufferPool获取空闲Buffer
  3. 调用Worker填充Buffer（使用`WorkerBase::fillBuffer()`方法）
  4. 提交填充后的Buffer
- `stop()`：停止生产流水线
- `getWorkingBufferPool()`：获取实际工作的BufferPool指针（供消费者使用，v2.0从Registry临时访问）
- `getWorkingBufferPoolId()`：获取工作BufferPool ID（v2.0新增方法）

**设计特点**：
- Worker必须创建BufferPool：Worker在实现`IVideoFileNavigator::open()`时通过调用Allocator创建BufferPool
- Registry中心化：Worker只记录pool_id，Registry独占持有BufferPool
- 临时访问：ProductionLine从Registry获取临时访问指针
- 线程安全：使用原子变量和互斥锁
- 错误处理：支持错误回调和错误信息查询，如果Worker没有创建BufferPool则start()失败

### 2. BufferPool（缓冲区池）

**职责**：
- 管理Buffer队列（空闲队列和填充队列）
- 提供线程安全的Buffer调度接口
- 不关心Buffer的来源和生命周期（由Allocator负责）

**关键成员变量**：
- `std::queue<Buffer*> free_queue_`：空闲队列
- `std::queue<Buffer*> filled_queue_`：填充队列
- `std::set<Buffer*> managed_buffers_`：所有托管的Buffer集合
- `std::mutex mutex_`：互斥锁
- `std::condition_variable free_cv_`：空闲队列条件变量
- `std::condition_variable filled_cv_`：填充队列条件变量
- `uint64_t registry_id_`：在BufferPoolRegistry中的注册ID

**核心方法**：
- `BufferPool(PrivateToken, name, category)`：构造函数（需要 Passkey Token，只有 Allocator 可以创建）
- `acquireFree(blocking, timeout_ms)`：获取空闲Buffer（生产者使用）
- `submitFilled(buffer)`：提交填充后的Buffer（生产者使用）
- `acquireFilled(blocking, timeout_ms)`：获取填充后的Buffer（消费者使用）
- `releaseFilled(buffer)`：归还Buffer到空闲队列（消费者使用）
- `getFreeCount()`、`getFilledCount()`、`getTotalCount()`：查询统计信息

**私有方法（仅供Allocator友元访问）**：
- `addBufferToQueue(buffer, queue)`：添加Buffer到队列
- `removeBufferFromPool(buffer)`：从Pool移除Buffer

**设计特点**：
- **Passkey Idiom**：通过 PrivateToken 限制创建权限，只有 Allocator 可以创建 BufferPool
- 纯调度器：只负责Buffer的调度，不负责创建和销毁
- 线程安全：所有操作使用互斥锁保护
- **注册机制**：所有BufferPool都注册到`BufferPoolRegistry`（使用 weak_ptr，不持有所有权）
  - Registry 使用 `weak_ptr<BufferPool>` 存储，不持有所有权
  - Pool 销毁时，Registry 的 `weak_ptr` 自动失效（expired）
  - 可通过 `weak_ptr::lock()` 临时提升为 `shared_ptr` 进行查询
- 友元关系：允许Allocator访问私有方法，保证封装性

### 3. WorkerBase（Worker统一基类）

**文件位置**:
- 基类: `include/productionline/worker/base/WorkerBase.hpp`

**架构角色**: 基类层（Base Layer）

**职责**：
- ✅ **定义Buffer填充功能**：通过纯虚函数定义契约（`fillBuffer()`, `getWorkerType()`, `getOutputBufferPoolId()`）
- ✅ **继承文件导航接口**：继承`IVideoFileNavigator`接口，提供文件操作功能
- ✅ **BufferPool创建**：Worker在`open()`时通过Allocator创建BufferPool，记录pool_id

**核心接口方法**（纯虚函数，子类必须实现）：
- `fillBuffer(frame_index, buffer)`：**核心功能**，填充Buffer
- `getOutputBufferPoolId()`：获取Worker的输出BufferPool ID（v2.0返回uint64_t）
- `getWorkerType()`：获取Worker类型名称（用于调试和日志）

**继承关系（v2.0）**：
- `WorkerBase`继承`IVideoFileNavigator`接口
- Worker实现类继承`WorkerBase`基类：`class FfmpegWorker : public WorkerBase`
- 简化架构：继承链为 IVideoFileNavigator → WorkerBase → 具体实现类

**设计特点**：
- ✅ **纯虚函数**：Buffer填充方法定义为纯虚函数，强制子类实现
- ✅ **Registry中心化**：Worker只记录pool_id，Registry独占持有BufferPool
- ✅ **依赖倒置**：上层代码依赖`WorkerBase`基类，不依赖具体实现

**注意**：
- Worker在实现`open()`时，需要同时处理文件打开逻辑和BufferPool创建逻辑（通过Allocator）
- 文件操作方法与Buffer填充操作分离，但都在WorkerBase中定义
- 所有Worker实现类（`FfmpegDecodeVideoFileWorker`, `MmapRawVideoFileWorker`, `FfmpegDecodeRtspWorker`, `IoUringRawVideoFileWorker`）都继承`WorkerBase`基类

### 4. IVideoFileNavigator（Worker文件导航接口）

**文件位置**:
- 接口: `include/productionline/worker/interface/IVideoFileNavigator.hpp`

**架构角色**: 接口层（Interface Layer）

**职责**：
- ✅ **定义契约**：定义所有Worker必须实现的文件操作接口
- ✅ **接口隔离**：专注于文件导航相关操作，与`IBufferFillingWorker`并列
- ✅ **职责分离**：文件操作与Buffer填充操作完全分离

**核心接口方法**（纯虚函数，子类必须实现）：
- **文件打开/关闭**：`open(path)`, `open(path, width, height, bits_per_pixel)`, `close()`, `isOpen()`
- **文件导航**：`seek()`, `seekToBegin()`, `seekToEnd()`, `skip()`
- **文件状态查询**：`getTotalFrames()`, `getCurrentFrameIndex()`, `getFrameSize()`, `getFileSize()`, `getWidth()`, `getHeight()`, `getBytesPerPixel()`, `getPath()`, `hasMoreFrames()`, `isAtEnd()`

**设计特点**：
- ✅ **纯虚函数**：所有方法都是纯虚函数，强制子类实现
- ✅ **接口分离**：与`IBufferFillingWorker`并列，职责清晰分离
- ✅ **依赖倒置**：上层代码依赖此接口，不依赖具体实现

**接口关系（v2.0）**：
- `IVideoFileNavigator` 是唯一的Worker接口
- Worker实现类通过继承 `WorkerBase` 基类来实现此接口
- `WorkerBase` 基类继承 `IVideoFileNavigator`，同时定义Buffer填充方法（纯虚函数）

### 5. BufferAllocatorBase（Allocator接口，纯抽象基类）

**文件位置**:
- 接口: `include/buffer/allocator/base/BufferAllocatorBase.hpp`
- 实现类: `include/buffer/allocator/implementation/`（NormalAllocator, AVFrameAllocator, FramebufferAllocator）

**架构角色**: 接口层（Interface Layer，纯抽象基类）

**职责**：
- ✅ **定义契约**：定义所有Allocator必须实现的接口
- ✅ **内存管理**：创建和销毁BufferPool、Buffer
- ✅ **生命周期管理**：管理Buffer的所有权

**核心接口方法**（纯虚函数，子类必须实现）：
- `allocatePoolWithBuffers(count, size, name, category)`：创建BufferPool并注入指定数量的Buffer
  - **返回类型**：`uint64_t`（返回 pool_id，Registry 持有 Pool）
  - **设计**：Allocator 创建后立即注册到 Registry，Registry 独占持有 BufferPool
  - **Registry**：自动注册到 BufferPoolRegistry（传入 Allocator ID，Registry 记录归属关系）
- `injectBufferToPool(pool_id, size, queue)`：将Buffer注入到BufferPool
- `removeBufferFromPool(pool_id, buffer)`：从BufferPool移除Buffer
- `destroyPool(pool_id)`：销毁整个BufferPool及其所有Buffer

**子类必须实现的核心方法**（protected，纯虚函数）：
- `createBuffer(id, size)`：创建单个Buffer（核心分配逻辑）
- `deallocateBuffer(buffer)`：销毁Buffer（核心释放逻辑）

**友元访问辅助方法**（供子类使用）：
- `static BufferPool::PrivateToken token()`：获取 Passkey Token，用于创建 BufferPool
  - 子类通过 `token()` 获取通行证
  - 调用 `std::make_unique<BufferPool>(token(), name, category)` 创建 BufferPool

**子类创建 BufferPool 的方式**：
```cpp
// 在子类的 allocatePoolWithBuffers() 中
// 1. 创建 BufferPool（shared_ptr）
auto pool = std::make_shared<BufferPool>(
    token(),    // 从基类获取通行证（Passkey Token）
    name,       // Pool 名称
    category    // Pool 分类
);

// 2. 注册到Registry（传入 Allocator ID，Registry 记录归属关系）
uint64_t pool_id = BufferPoolRegistry::getInstance().registerPool(pool, getAllocatorId());
pool->setRegistryId(pool_id);

// 3. 返回 pool_id（Registry 独占持有 Pool）
return pool_id;
```

**设计变更说明（v2.0）**：
- ✅ **Registry 中心化管理**：Registry 独占持有 BufferPool（shared_ptr，引用计数=1）
- ✅ **Allocator ID 机制**：每个 Allocator 有唯一 ID，Registry 记录 Pool 的创建者
- ✅ **Allocator 不维护状态**：Allocator 不持有 Pool 列表，需要时向 Registry 查询
- ✅ **自动清理**：Allocator 析构时自动查询 Registry 获取所有 Pool，逐个清理
- ✅ **Worker 主动清理**：Worker 的 `close()` 调用 `destroyPool()` 主动清理资源

**设计特点**：
- ✅ **纯抽象接口**：所有方法都是纯虚函数（`= 0`），只有头文件，无实现文件
- ✅ **接口契约**：定义所有Allocator必须实现的完整接口
- ✅ **依赖倒置**：上层代码依赖 `BufferAllocatorBase` 接口，不依赖具体实现
- ✅ **友元关系**：是 BufferPool::PrivateToken 的 friend，可以创建通行证
- ✅ **Passkey 控制**：通过 `token()` 方法向子类提供创建 BufferPool 的能力
- ✅ **实现透明**：具体实现类对上层完全透明

**注意**：
- Worker在`open()`时通过`BufferAllocatorFacade`调用Allocator创建BufferPool
- Allocator是唯一可以创建和销毁Buffer的组件
- BufferPool 只能通过 Allocator（持有 Token）创建，外部无法直接创建
- **所有权设计**：
  - Allocator 创建 BufferPool 后立即返回 `unique_ptr`，不持有所有权
  - Worker 持有 `unique_ptr`，通过 `getOutputBufferPool()` 转移给 ProductionLine
  - Registry 使用 `weak_ptr` 观察，不持有所有权
  - **谁持有谁释放**：持有 `unique_ptr` 的组件负责释放 BufferPool（RAII 原则）

### 6. WorkerBase（Worker统一基类）

**文件位置**:
- 基类: `include/productionline/worker/base/WorkerBase.hpp`

**架构角色**: 基类层（Base Layer）

**职责**：
- ✅ **统一基类**：作为所有Worker实现类的统一基类
- ✅ **接口继承**：继承 `IVideoFileNavigator` 接口
- ✅ **定义Buffer填充功能**：通过纯虚函数定义Buffer填充方法
- ✅ **公共功能**：提供所有Worker共同的公共功能（Allocator、BufferPool管理）
- ✅ **类型统一**：提供统一的类型系统，便于工厂模式和门面模式使用

**继承关系（v2.0）**：
- `WorkerBase` 继承 `IVideoFileNavigator`
- 所有具体Worker实现类继承 `WorkerBase`

**核心成员**（protected，子类自动继承）：
- `BufferAllocatorFacade allocator_facade_`：Allocator门面（所有Worker自动继承）
- `uint64_t buffer_pool_id_`：Worker创建的BufferPool ID（v2.0：Registry独占持有）

**核心方法**（public，纯虚函数，子类必须实现）：
- `fillBuffer(frame_index, buffer)`：填充Buffer（纯虚函数）
- `getOutputBufferPoolId()`：返回 pool_id（默认实现，子类可重写）
- `getWorkerType()`：获取Worker类型名称（纯虚函数）
- 所有 `IVideoFileNavigator` 接口方法（纯虚函数）

**设计特点**：
- ✅ **类型安全**：不需要dynamic_cast，直接使用基类指针即可访问接口
- ✅ **代码简洁**：门面类只需要一个worker_指针
- ✅ **统一管理**：所有Worker自动继承allocator_和buffer_pool_id_，无需每个子类重复定义
- ✅ **Registry中心化**：Worker只记录pool_id，Registry独占持有BufferPool
- ✅ **架构清晰**：明确的继承层次，符合面向对象设计原则
- ✅ **易于维护**：统一的基类便于扩展和维护

**优势**：
- Factory返回`WorkerBase*`，统一类型系统
- Facade持有`WorkerBase*`，直接访问所有方法
- 子类只需实现纯虚函数，无需管理Allocator和BufferPool的创建逻辑

### 7. BufferFillingWorkerFacade（Worker门面）

**文件位置**:
- 门面: `include/productionline/worker/facade/BufferFillingWorkerFacade.hpp`

**架构角色**: 门面层（Facade Layer）

**职责**：
- 为用户提供统一、简单的Buffer填充操作接口
- 隐藏底层多种实现的复杂性
- 自动选择最优的Worker实现
- **实现 `IBufferFillingWorker` 和 `IVideoFileNavigator` 两个接口**，确保类型安全和API一致性

**关键成员变量**：
- `std::unique_ptr<WorkerBase> worker_`：实际的Worker实现（统一基类）
- `WorkerType preferred_type_`：用户偏好的Worker类型

**核心方法**：
- `open(path)`：打开编码视频文件（自动检测格式）
- `open(path, width, height, bpp)`：统一智能接口
  - 根据Worker类型自动判断参数用途
  - 编码视频：忽略width/height/bpp，自动检测格式
  - Raw视频：使用width/height/bpp参数
- `fillBuffer(frame_index, buffer)`：填充Buffer（转发到底层Worker）
- `getOutputBufferPoolId()`：获取Worker的输出BufferPool ID（v2.0返回uint64_t）
- 所有方法不使用 `override` 关键字（v2.1不继承接口）

**设计特点（v2.1）**：
- **组合模式**：不继承接口，通过持有 WorkerBase 指针实现方法转发
- 门面模式：简化复杂子系统接口
- 智能判断：根据Worker类型自动处理参数
- 使用WorkerBase：直接通过worker_base_uptr_转发所有方法
- Registry访问：getOutputBufferPoolId()返回pool_id，调用者从Registry获取临时访问
- **架构简化**：减少继承层次，提升灵活性

### 7. BufferFillingWorkerFactory（工厂）

**职责**：
- 根据环境和配置创建合适的Worker实现
- 封装Worker创建逻辑
- 支持自动检测和手动指定两种模式

**核心方法**：
- `create(WorkerType)`：工厂方法，创建Worker实例
- `createByName(name)`：通过名称创建Worker
- `autoDetect()`：自动检测最优Worker
- `createByType(type)`：根据类型创建Worker

**创建策略**（优先级从高到低）：
1. 用户显式指定（`type != AUTO`）
2. 环境变量（`VIDEO_READER_TYPE`）
3. 配置文件（`/etc/video_reader.conf`）
4. 自动检测系统能力

### 8. Allocator实现类（Implementation Layer）

**文件位置**:
- 实现类: `include/buffer/allocator/implementation/`（NormalAllocator, AVFrameAllocator, FramebufferAllocator）

**架构角色**: 实现层（Implementation Layer）

**设计特点**：
- ✅ **继承接口**：所有实现类继承 `BufferAllocatorBase` 接口
- ✅ **实现契约**：实现所有接口定义的纯虚函数
- ✅ **对上层透明**：上层代码通过接口访问，不依赖具体实现类

**实现类概览**：
- **NormalAllocator**：普通内存分配器（malloc/posix_memalign），适用于CPU处理的普通数据缓冲
- **AVFrameAllocator**：AVFrame包装分配器（FFmpeg帧内存），适用于FFmpeg解码，零拷贝模式
- **FramebufferAllocator**：Framebuffer内存包装分配器（外部内存），适用于Framebuffer设备

**关键设计**：
- ✅ **接口统一**：所有实现类通过 `BufferAllocatorBase` 接口统一访问
- ✅ **工厂创建**：通过 `BufferAllocatorFactory` 创建，返回接口指针
- ✅ **实现透明**：具体实现细节对上层完全透明

---

## 使用示例

### 示例1：基本使用（Worker自动创建BufferPool）

```cpp
#include "productionline/VideoProductionLine.hpp"
#include "buffer/BufferPool.hpp"
#include "buffer/BufferPoolRegistry.hpp"

int main() {
    // 1. 创建VideoProductionLine（Worker会在实现IVideoFileNavigator::open()时自动创建BufferPool）
    VideoProductionLine producer;
    
    // 2. 配置视频源
    VideoProductionLine::Config config(
        "/path/to/video.raw",  // 文件路径
        1920,                  // 宽度
        1080,                  // 高度
        32,                    // 每像素位数（ARGB888）
        true,                  // 循环播放
        2,                     // 2个生产者线程
        BufferFillingWorkerFactory::WorkerType::MMAP_RAW  // Worker类型
    );
    
    // 3. 启动生产流水线（Worker会在实现IVideoFileNavigator::open()时自动调用Allocator创建BufferPool）
    if (!producer.start(config)) {
        printf("Failed to start production line\n");
        return -1;
    }
    
    // 4. 获取工作BufferPool ID（v2.0）
    uint64_t pool_id = producer.getWorkingBufferPoolId();
    auto& registry = BufferPoolRegistry::getInstance();
    auto pool = registry.getPool(pool_id);
    if (!pool) {
        printf("❌ Pool not found\n");
        return -1;
    }
    
    // 6. 消费者循环：从BufferPool获取填充后的Buffer
    while (running) {
        Buffer* filled_buffer = pool->acquireFilled(true, 100);
        if (filled_buffer) {
            // 处理Buffer（显示、分析等）
            processBuffer(filled_buffer);
            
            // 归还Buffer
            pool->releaseFilled(filled_buffer);
        }
    }
    
    // 7. 停止生产流水线
    producer.stop();
    
    return 0;
}
```

### 示例2：RTSP流（零拷贝模式）

```cpp
// 1. 创建VideoProductionLine（Worker会在实现IVideoFileNavigator::open()时自动创建BufferPool）
VideoProductionLine producer;

// 2. 配置RTSP Worker（Worker会在实现IVideoFileNavigator::open()时自动调用Allocator创建BufferPool）
VideoProductionLine::Config config(
    "rtsp://192.168.1.100:8554/stream",  // RTSP URL
    1920, 1080, 32,                      // 输出分辨率
    false,                               // 不循环（RTSP流是无限的）
    1,                                   // 单线程（RTSP推荐）
    BufferFillingWorkerFactory::WorkerType::FFMPEG_RTSP
);

// 4. 启动（Worker内部会自动注入Buffer到pool）
producer.start(config);

// 5. 获取工作BufferPool ID（v2.0）
uint64_t pool_id = producer.getWorkingBufferPoolId();
auto& registry = BufferPoolRegistry::getInstance();
auto pool = registry.getPool(pool_id);

// 6. 消费者循环（Worker已自动注入Buffer，直接使用即可）
while (running) {
    Buffer* buffer = pool->acquireFilled(true, 100);
    if (buffer) {
        // 零拷贝显示（使用DMA）
        display.displayBufferByDMA(buffer);
        pool->releaseFilled(buffer);
    }
}
```

---

## 最佳实践

### 1. 选择正确的Worker类型

| 场景 | Worker类型 | Worker内部使用的Allocator | 理由 |
|------|-----------|-------------------------|------|
| Raw视频文件（小文件） | `MMAP_RAW` | NormalAllocator（Worker自动选择） | 实现简单，随机访问性能优秀 |
| Raw视频文件（大文件） | `IOURING_RAW` | NormalAllocator（Worker自动选择） | 零拷贝异步I/O，提高吞吐量 |
| 编码视频文件 | `FFMPEG_VIDEO_FILE` | NormalAllocator（Worker自动选择） | 支持多种编码格式，硬件加速 |
| RTSP流 | `FFMPEG_RTSP` | AVFrameAllocator（Worker自动选择） | 实时流处理，零拷贝模式 |

### 2. BufferPool创建策略

**推荐做法：**
- ✅ 使用Worker自动创建BufferPool，Worker会根据场景自动选择合适的Allocator
- ✅ Worker必须在实现`IVideoFileNavigator::open()`时创建BufferPool，不能返回nullptr
- ❌ 不要直接调用Allocator创建BufferPool（除非是不涉及Worker的场景）

### 3. 错误处理

```cpp
// ✅ 推荐：检查返回值
Buffer* buf = pool->acquireFree(true, 100);  // 超时100ms
if (!buf) {
    // 超时或pool已销毁
    LOG_ERROR("Failed to acquire buffer");
    return;
}

// ✅ 推荐：使用RAII自动归还
class ScopedBuffer {
public:
    ScopedBuffer(BufferPool* pool, Buffer* buf) 
        : pool_(pool), buf_(buf) {}
    
    ~ScopedBuffer() {
        if (buf_) pool_->releaseFilled(buf_);
    }
    
    Buffer* get() { return buf_; }
    
private:
    BufferPool* pool_;
    Buffer* buf_;
};

// 使用
{
    ScopedBuffer scoped(pool.get(), pool->acquireFilled(true));
    if (scoped.get()) {
        // 使用buffer
    }
}  // 自动归还
```

### 4. 性能优化

#### 优化1：避免频繁加锁

```cpp
// ❌ 不推荐：频繁查询
while (true) {
    if (pool->getFilledCount() > 0) {  // 加锁
        Buffer* buf = pool->acquireFilled(false);  // 再次加锁
        // ...
    }
}

// ✅ 推荐：使用阻塞等待（条件变量，高效）
while (true) {
    Buffer* buf = pool->acquireFilled(true, 100);  // 一次加锁+等待
    if (buf) {
        // ...
    }
}
```

#### 优化2：预分配足够的buffer

```cpp
// ✅ 推荐：根据处理速度比例分配
// 例如：解码30fps，显示60fps → 需要30/60 = 0.5倍buffer
// 实际应该分配10-20个buffer留有余地
```

### 5. 线程安全注意事项

```cpp
// ✅ BufferPool所有接口都是线程安全的
// 可以在多个线程中直接调用，无需额外加锁

// ✅ Buffer对象的数据访问需要自己保证线程安全
Buffer* buf = pool->acquireFree(true);
// 此时buf在生产者手中，只有这个线程可以访问buf->getVirtualAddress()

pool->submitFilled(buf);
// buf已经提交，生产者不应再访问buf->getVirtualAddress()

// ⚠️ 注意：不要在持有buffer期间销毁BufferPool
// 应该先归还所有buffer，再销毁Pool
```

---

## 代码规范与风格指南

### 概述

本文档遵循业界主流 C++ 代码规范，确保代码风格统一、可读性强、易于维护。所有代码调整都基于以下大厂规范：

- **Google C++ Style Guide**：类成员顺序、访问控制规范
- **LLVM Coding Standards**：方法分组、注释规范
- **Microsoft C++ Guidelines**：接口与实现分离原则

### 类成员访问控制顺序

#### 基本原则

**推荐顺序：`public` → `protected` → `private`**

这是业界主流规范，符合"接口优先"的设计理念：
- 用户最关心的是公共接口（public）
- 子类需要了解受保护接口（protected）
- 实现细节放在最后（private）

#### 每个访问级别内的顺序

在每个访问级别内，按以下顺序组织：

```
public:
    // 1. 类型别名和常量（public）
    using TypeAlias = ...;
    static constexpr int CONSTANT = ...;
    
    // 2. 构造函数和析构函数
    MyClass();
    ~MyClass();
    
    // 3. 核心公共接口（按功能分组）
    void publicMethod1();
    void publicMethod2();
    
protected:
    // 受保护接口（供子类使用）
    
private:
    // 私有实现细节
    // 1. 成员变量（通常放在最后）
    // 2. 私有辅助方法
```

#### 实际调整案例

在本次代码重构中，我们调整了以下文件的成员顺序：

| 文件 | 调整内容 | 调整原因 |
|------|---------|---------|
| `PerformanceMonitor.hpp` | 将 `private` 移到 `public` 之后 | 符合 public → private 顺序 |
| `LinuxFramebufferDevice.hpp` | 将 `private` 移到 `public` 之后 | 符合 public → private 顺序 |
| `FfmpegDecodeRtspWorker.hpp` | 将 `private` 移到 `public` 之后 | 符合 public → private 顺序 |
| `MmapRawVideoFileWorker.hpp` | 将 `private` 移到 `public` 之后 | 符合 public → private 顺序 |
| `FfmpegDecodeVideoFileWorker.hpp` | 将 `private` 移到 `public` 之后 | 符合 public → private 顺序 |

**调整前示例**：
```cpp
class PerformanceMonitor {
private:
    // 成员变量和辅助方法
    mutable std::mutex mutex_;
    int frames_loaded_;
    // ...
    
public:
    // 公共接口
    PerformanceMonitor();
    void start();
    // ...
};
```

**调整后示例**：
```cpp
class PerformanceMonitor {
public:
    // 公共接口
    PerformanceMonitor();
    ~PerformanceMonitor();
    void start();
    // ...
    
private:
    // 成员变量和辅助方法
    mutable std::mutex mutex_;
    int frames_loaded_;
    // ...
};
```

### 方法分组原则

#### 功能分组

公共方法应按功能分组，使用注释分隔：

```cpp
public:
    // ============ 构造/析构 ============
    MyClass();
    ~MyClass();
    
    // ============ 核心业务接口 ============
    // 4.1 Buffer填充相关
    bool fillBuffer(...);
    uint64_t getOutputBufferPoolId();
    
    // 4.2 文件操作相关
    bool open(...);
    void close();
    
    // 4.3 导航操作相关
    bool seek(...);
    bool skip(...);
    
    // 4.4 状态查询相关
    int getTotalFrames() const;
    int getCurrentFrameIndex() const;
    
private:
    // ============ 实现细节 ============
    std::unique_ptr<WorkerBase> worker_;
    int preferred_type_;
    
    // ============ 内部辅助方法 ============
    void validateInput();
    bool initialize();
```

#### 分组建议

1. **生命周期管理**：构造函数、析构函数、初始化、清理
2. **核心业务接口**：按功能模块分组（如 Buffer 填充、文件操作、导航操作）
3. **查询接口**：所有 `get*()`、`is*()`、`has*()` 方法放在一起
4. **修改接口**：所有 `set*()`、`open()`、`close()` 方法放在一起
5. **错误处理**：错误回调、错误信息查询
6. **调试接口**：统计信息、打印方法

### 成员变量组织

#### 私有成员变量顺序

私有成员变量建议按以下顺序组织：

1. **资源管理**：智能指针、文件描述符、句柄等
2. **状态信息**：原子变量、标志位、计数器等
3. **配置参数**：用户配置、系统参数等
4. **辅助数据**：临时变量、缓存等

**示例**：
```cpp
private:
    // ============ 资源管理 ============
    std::unique_ptr<WorkerBase> worker_;
    int fd_;
    
    // ============ 状态信息 ============
    std::atomic<bool> running_;
    bool is_open_;
    int current_frame_index_;
    
    // ============ 配置参数 ============
    int width_;
    int height_;
    int bits_per_pixel_;
    
    // ============ 辅助数据 ============
    std::string last_error_;
    mutable std::mutex error_mutex_;
```

### 代码风格检查清单

在提交代码前，请检查以下事项：

- [ ] 访问控制顺序：`public` → `protected` → `private`
- [ ] 构造函数和析构函数在 `public` 区域最前面
- [ ] 方法按功能分组，使用注释分隔
- [ ] 成员变量在 `private` 区域最后
- [ ] 相关方法放在一起（如所有 `get*()` 方法）
- [ ] 禁止拷贝的声明紧跟在析构函数之后

### 参考规范

- **Google C++ Style Guide**: [Class Format](https://google.github.io/styleguide/cppguide.html#Class_Format)
- **LLVM Coding Standards**: [Class Organization](https://llvm.org/docs/CodingStandards.html#class-organization)
- **Microsoft C++ Guidelines**: [Class Design](https://docs.microsoft.com/en-us/cpp/cpp/class-design)

### 实际应用

本项目中的所有类都应遵循以上规范。在代码审查时，应检查：

1. ✅ 访问控制顺序是否正确
2. ✅ 方法是否按功能分组
3. ✅ 成员变量是否合理组织
4. ✅ 注释是否清晰明确

通过统一的代码风格，可以：
- **提升可读性**：新成员可以快速理解代码结构
- **便于维护**：相关功能集中，修改更容易
- **符合规范**：遵循业界主流标准，代码质量更高

---

## API参考

### VideoProductionLine API

| 方法 | 说明 | 参数 | 返回值 |
|------|------|------|--------|
| `VideoProductionLine()` | 构造函数 | 无 | 无 |
| `start(config)` | 启动生产流水线 | `config`: 配置结构体 | `bool` |
| `stop()` | 停止生产流水线 | 无 | 无 |
| `getWorkingBufferPool()` | 获取实际工作的BufferPool指针（v2.0：从Registry获取临时访问） | 无 | `BufferPool*` |
| `getWorkingBufferPoolId()` | 获取工作BufferPool ID（v2.0新增） | 无 | `uint64_t` |

### BufferPool API

| 方法 | 说明 | 参数 | 返回值 |
|------|------|------|--------|
| `BufferPool(token, name, category)` | 构造函数（需要 Passkey Token） | `token`: PrivateToken 通行证<br>`name`: Pool名称<br>`category`: Pool分类 | 无 |
| `acquireFree(blocking, timeout_ms)` | 获取空闲buffer | `blocking`: 是否阻塞<br>`timeout_ms`: 超时（毫秒） | `Buffer*`（失败返回nullptr） |
| `submitFilled(buffer)` | 提交填充buffer | `buffer`: 已填充的buffer | 无 |
| `acquireFilled(blocking, timeout_ms)` | 获取就绪buffer | `blocking`: 是否阻塞<br>`timeout_ms`: 超时（毫秒） | `Buffer*`（失败返回nullptr） |
| `releaseFilled(buffer)` | 归还buffer | `buffer`: 已使用的buffer | 无 |
| `getFreeCount()` | 获取空闲buffer数量 | 无 | `int` |
| `getFilledCount()` | 获取就绪buffer数量 | 无 | `int` |
| `getTotalCount()` | 获取总buffer数量 | 无 | `int` |

**注意**：BufferPool 只能通过 Allocator（持有 Passkey Token）创建，外部无法直接实例化。

### BufferAllocator API

| 方法 | 说明 | 参数 | 返回值 |
|------|------|------|--------|
| `allocatePoolWithBuffers(count, size, name, category)` | 批量分配并创建Pool | `count`: Buffer数量<br>`size`: Buffer大小<br>`name`: Pool名称<br>`category`: Pool分类 | `uint64_t`<br>（返回pool_id，Registry独占持有Pool） |
| `injectBufferToPool(pool_id, size, queue)` | 单个注入到Pool | `pool_id`: BufferPool ID<br>`size`: Buffer大小<br>`queue`: 队列类型 | `Buffer*` |
| `removeBufferFromPool(pool_id, buffer)` | 从Pool移除并销毁 | `pool_id`: BufferPool ID<br>`buffer`: Buffer指针 | `bool` |
| `destroyPool(pool_id)` | 销毁整个Pool及其所有Buffer | `pool_id`: BufferPool ID | `bool` |

**所有权说明（v2.0）**：
- ✅ `allocatePoolWithBuffers()` 返回 `uint64_t` pool_id，Registry独占持有BufferPool
- ✅ Allocator 创建后立即注册到Registry，不持有BufferPool
- ✅ Registry 负责BufferPool生命周期管理（shared_ptr，引用计数=1）
- ✅ 调用者通过 `BufferPoolRegistry::getInstance().getPool(pool_id)` 获取临时访问

### WorkerBase API

| 方法 | 说明 | 参数 | 返回值 |
|------|------|------|--------|
| `fillBuffer(frame_index, buffer)` | 填充Buffer（核心功能，纯虚函数） | `frame_index`: 帧索引<br>`buffer`: Buffer指针 | `bool` |
| `getOutputBufferPoolId()` | 获取Worker的输出BufferPool ID（v2.0） | 无 | `uint64_t`（0表示未创建） |
| `getWorkerType()` | 获取Worker类型名称 | 无 | `const char*` |

**注意（v2.0）**：
- ✅ 文件操作方法（`open()`, `close()`, `isOpen()`）属于`IVideoFileNavigator`接口，WorkerBase继承此接口
- ✅ `open()`方法有两个重载版本
- ✅ Worker必须在`open()`时创建BufferPool，否则返回0
- ✅ 调用者通过 `BufferPoolRegistry::getInstance().getPool(pool_id)` 获取临时访问

### IVideoFileNavigator API

| 方法 | 说明 | 参数 | 返回值 |
|------|------|------|--------|
| **文件打开/关闭** |
| `open(path)` | 打开编码视频文件（自动检测格式） | `path`: 文件路径 | `bool` |
| `open(path, w, h, bpp)` | 打开视频文件（统一智能接口） | `path`: 文件路径<br>`w`: 宽度（可选，用于raw视频）<br>`h`: 高度（可选，用于raw视频）<br>`bpp`: 每像素位数（可选，用于raw视频） | `bool` |
| `close()` | 关闭视频文件 | 无 | `void` |
| `isOpen()` | 检查文件是否已打开 | 无 | `bool` |
| **文件导航** |
| `seek(frame_index)` | 跳转到指定帧 | `frame_index`: 帧索引 | `bool` |
| `seekToBegin()` | 回到文件开头 | 无 | `bool` |
| `seekToEnd()` | 跳转到文件末尾 | 无 | `bool` |
| `skip(frame_count)` | 跳过N帧（可正可负） | `frame_count`: 跳过的帧数 | `bool` |
| **文件状态查询** |
| `getTotalFrames()` | 获取总帧数 | 无 | `int` |
| `getCurrentFrameIndex()` | 获取当前帧索引 | 无 | `int` |
| `getFrameSize()` | 获取单帧大小（字节） | 无 | `size_t` |
| `getFileSize()` | 获取文件大小（字节） | 无 | `long` |
| `getWidth()` | 获取视频宽度 | 无 | `int` |
| `getHeight()` | 获取视频高度 | 无 | `int` |
| `getBytesPerPixel()` | 获取每像素字节数 | 无 | `int` |
| `getPath()` | 获取文件路径 | 无 | `const char*` |
| `hasMoreFrames()` | 检查是否还有更多帧 | 无 | `bool` |
| `isAtEnd()` | 检查是否到达文件末尾 | 无 | `bool` |

---

## 常见问题

### Q1: Worker和ProductionLine的区别是什么？

**A**: 
- **Worker**：负责填充Buffer的具体实现（工人），从数据源获取数据并填充到Buffer
- **ProductionLine**：负责管理生产流程（生产流水线），协调Buffer的获取、填充、提交
- **关系**：ProductionLine使用Worker来填充Buffer，Worker提供原材料（BufferPool）给ProductionLine

### Q2: Worker如何创建BufferPool？

**A**:
- **Worker必须创建BufferPool**：Worker在实现`IVideoFileNavigator::open()`时**必须**自动调用Allocator创建BufferPool，流程如下：
  1. Worker实现`IVideoFileNavigator::open()`方法（文件打开逻辑）
  2. Worker创建Allocator实例（根据场景选择合适的Allocator）
     - Raw视频文件：使用NormalAllocator
     - RTSP流/编码视频：使用AVFrameAllocator（动态注入模式）
     - Framebuffer显示：使用FramebufferAllocator
  3. Worker调用`allocator->allocatePoolWithBuffers(count, size, name, category)`
  4. Allocator内部：
     - 使用 Passkey Token 创建空的 BufferPool：
       ```cpp
       auto pool = std::make_unique<BufferPool>(token(), name, category);
       ```
       - `token()` 从 `BufferAllocatorBase` 基类获取通行证
       - 只有 Allocator 可以创建 `PrivateToken`
     - 注册到Registry（使用weak_ptr，不持有所有权）：
       ```cpp
       std::shared_ptr<BufferPool> temp_shared = std::shared_ptr<BufferPool>(
           pool.get(), [](BufferPool*) {}  // 空删除器
       );
       uint64_t id = BufferPoolRegistry::getInstance().registerPoolWeak(temp_shared);
       pool->setRegistryId(id);
       temp_shared.reset();  // 释放临时shared_ptr
       ```
     - 循环创建Buffer：调用子类的`createBuffer(id, size)`
     - 注入Buffer到Pool：通过友元关系调用`BufferPool::addBufferToQueue(buffer, FREE)`
     - 返回`unique_ptr<BufferPool>`（转移所有权给Worker）
  5. Worker保存创建的BufferPool（内部成员 `buffer_pool_uptr_`）
  6. Worker通过`getOutputBufferPool()`返回创建的BufferPool（转移所有权给ProductionLine）
- **关键点**：
  - Worker通过调用Allocator创建BufferPool，而不是直接创建
  - `open()`方法属于`IVideoFileNavigator`接口，但Worker在实现时需要同时处理文件打开和BufferPool创建
  - 如果Worker返回nullptr，ProductionLine的`start()`会失败并报错："Worker failed to create BufferPool"

### Q3: Allocator和BufferPool的关系是什么？

**A**:
- **Allocator**：负责Buffer和BufferPool的创建和销毁（内存管理）
- **BufferPool**：负责Buffer的队列调度（调度管理）
- **关系**：Allocator是BufferPool的友元类，可以访问BufferPool的私有方法（`addBufferToQueue()`、`removeBufferFromPool()`）
- **设计原则**：遵循单一职责原则，Allocator只负责内存管理，BufferPool只负责调度

### Q4: 如何选择合适的Worker类型？

**A**:
- **Raw视频文件**：
  - 小文件（<1GB）：`MMAP_RAW`
  - 大文件、高并发：`IOURING_RAW`
- **编码视频文件**：`FFMPEG_VIDEO_FILE`
- **RTSP流**：`FFMPEG_RTSP`
- **自动选择**：`AUTO`（工厂会自动检测最优类型）

### Q5: 多线程生产时如何保证线程安全？

**A**:
- **帧索引**：使用`std::atomic<int> next_frame_index_`原子递增
- **Worker**：使用`std::shared_ptr`多线程共享（只读操作）
- **BufferPool**：内部实现线程安全（使用互斥锁）
- **统计信息**：使用`std::atomic`原子变量

---

## 总结

ProductionLine架构通过清晰的职责划分和设计模式应用，实现了：

1. **高内聚低耦合**：每个类职责单一，依赖接口而非实现
2. **接口分离**：`IBufferFillingWorker`和`IVideoFileNavigator`并列，职责清晰分离
3. **易于扩展**：新增Worker只需实现接口，无需修改现有代码
4. **灵活配置**：支持多种Worker类型，自动或手动选择
5. **性能优化**：支持零拷贝、异步I/O等高性能模式
6. **线程安全**：多线程生产支持，原子操作保证线程安全

通过"生产流水线"和"工人"的类比，开发者可以直观地理解整个架构的设计逻辑和数据流向。

**接口职责分离（v2.0）**：
- `IVideoFileNavigator`：专注于文件相关操作（`open()`的两个重载版本, `close()`, `seek()`, `getTotalFrames()`等）
- `WorkerBase`：继承`IVideoFileNavigator`，定义Buffer填充方法（`fillBuffer()`, `getOutputBufferPoolId()`, `getWorkerType()`等）
- Worker实现类通过继承 `WorkerBase` 基类实现所有纯虚函数，职责清晰，符合单一职责原则（SRP）
- `BufferFillingWorkerFacade` 门面类（v2.1）不继承接口，通过组合模式转发方法，简化架构

**BufferPool 创建权限控制（v2.0）**：
- 采用 **Passkey Idiom**（通行证模式）限制 BufferPool 的创建权限
- 只有 Allocator（持有 PrivateToken）可以创建 BufferPool 实例
- 提供比 friend 更精细的访问控制，更加安全和优雅
- 子类通过 `BufferAllocatorBase::token()` 获取通行证，调用 `std::make_shared<BufferPool>(token(), name, category)` 创建

**BufferPool 所有权管理（v2.0）**：
- **Registry 中心化管理**：Registry 独占持有 BufferPool（shared_ptr，引用计数=1）
- **Allocator ID 机制**：每个 Allocator 有唯一 ID，Registry 记录 Pool 归属关系
- **Allocator 不维护状态**：Allocator 不持有 Pool 列表，需要时向 Registry 查询
- **自动清理**：Allocator 析构时查询 Registry 获取所有 Pool，逐个调用 `destroyPool()` 清理
- **Worker 主动清理**：Worker 的 `close()` 调用 `destroyPool()` 主动清理资源
- **生命周期清晰**：Pool 销毁时，Registry 自动从归属关系中移除

**代码规范与风格**：
- **访问控制顺序**：遵循 `public` → `protected` → `private` 顺序，符合 Google C++ Style Guide、LLVM Coding Standards 等业界规范
- **方法分组**：按功能组织方法（生命周期、核心接口、查询接口等），使用注释分隔
- **成员变量组织**：私有成员变量按资源管理、状态信息、配置参数、辅助数据的顺序组织
- **统一风格**：所有类遵循统一的代码风格，提升可读性和可维护性

---

**文档维护：** AI SDK Team  
**最后更新：** 2025-12-05  
**架构版本：** v2.1（删除 IBufferFillingWorker 接口，BufferFillingWorkerFacade 不继承接口 + v2.0 Registry 中心化管理）  
**代码规范版本：** v1.0（统一类成员访问控制顺序为 public → private，遵循大厂代码规范）
