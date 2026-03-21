# TacoVOFramebufferDevice 多通道显示系统设计方案

## 1. 背景与目标

### 1.1 现状问题

当前 `taco-vo` 库存在以下缺陷：

1. **4-buffer 设计浪费**：虽然分配了 4 个 framebuffer，但 `front_buf_index` / `back_buf_index` 的交替使用实际上只实现了双缓冲，浪费了 2 个 buffer。
2. **耦合度高**：`taco-vo` 自成体系（Device → Layer → Channel），与组件系统的 `BufferPool`、`IBufferConsumer` 等机制无法融合。
3. **同步原语粗放**：通道发送帧时全局加锁 (`mutex`)，无法利用 PP 硬件的并发能力。
4. **内存管理不统一**：`taco-vo` 内部用 `ta_vo_frame` / `ta_avframe_t` 等私有结构管理 DMA 内存，与组件系统的 `Buffer` 概念割裂。

### 1.2 设计目标

将 `taco-vo` 的**全部功能**用组件系统架构重写，具体为：

| 目标 | 描述 |
|------|------|
| 多通道并发写入 | N 个 `VideoProductionLine` 对应 N 个显示通道，每通道通过 PP 硬件并发写入同一个 framebuffer |
| BufferPool 管理 framebuffer | 用组件系统的 `BufferPool`（FREE/FILLED 队列）管理 4 个 framebuffer 页，实现真正的多缓冲 |
| 定时器驱动显示 | 固定帧率定时器触发翻页显示，显示节奏与通道解码速度解耦 |
| 单次初始化/销毁 | DSS/USB-HDMI 设备的生命周期由 `SharedDisplayContext` 单例管理，不随通道数变化 |
| 测试层兼容 | 保持 `DisplayConsumer::consume(Buffer*)` 调用模式不变 |
| 零帧丢失 | 通道写入失败时缓存帧，下一周期重试 |

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              测试层 / 用户层                                 │
│                                                                             │
│   VideoProductionLine[0]   VideoProductionLine[1]   VideoProductionLine[N]  │
│         ↓ BufferPool            ↓ BufferPool             ↓ BufferPool       │
│    (解码帧 FREE/FILLED)    (解码帧 FREE/FILLED)    (解码帧 FREE/FILLED)     │
│         ↓                       ↓                        ↓                  │
│   DisplayConsumer[0]       DisplayConsumer[1]       DisplayConsumer[N]      │
│   (IBufferConsumer)        (IBufferConsumer)        (IBufferConsumer)       │
└────────┬────────────────────────┬────────────────────────┬──────────────────┘
         │ consume()              │ consume()              │ consume()
         ↓                        ↓                        ↓
┌─────────────────────────────────────────────────────────────────────────────┐
│                        SharedDisplayContext（单例）                           │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │  Framebuffer BufferPool（4 个 Buffer，DMA 物理连续内存）              │    │
│  │                                                                     │    │
│  │  Buffer[0]  Buffer[1]  Buffer[2]  Buffer[3]                        │    │
│  │  FREE ←──→ FILLED（显示线程消费）                                    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                                                             │
│  ┌──────────────────┐   ┌──────────────────┐   ┌────────────────────────┐  │
│  │  render_buf_      │   │  display_buf_     │   │  std::shared_mutex     │  │
│  │  (当前渲染目标)    │   │  (当前显示中)      │   │  rw_mutex_             │  │
│  └──────────────────┘   └──────────────────┘   └────────────────────────┘  │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                       timerfd 定时器线程                               │   │
│  │  33ms 周期 → 独占锁 → 提交旧buffer → 获取新buffer → PP拷贝 → 释放锁  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        显示线程 (display thread)                      │   │
│  │  acquireFilled → FBIOPAN_DISPLAY → FBIO_WAITFORVSYNC → releaseFilled │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
         │
         ↓
┌─────────────────────────────────────────────────────────────────────────────┐
│                         硬件层                                               │
│                                                                             │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐  ┌────────────────┐  │
│  │ PP 硬件引擎  │  │ DSS 显示控制  │  │ tpsfb 驱动   │  │ HDMI / LCD     │  │
│  │ (resize/CSC)│  │ (DMA 读取)    │  │ (/dev/fbX)   │  │ (物理输出)      │  │
│  └─────────────┘  └──────────────┘  └──────────────┘  └────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 核心类设计

### 3.1 SharedDisplayContext（单例 - 共享显示上下文）

管理所有通道共享的显示资源。通过引用计数实现单次初始化/销毁。

```cpp
class SharedDisplayContext {
public:
    static std::shared_ptr<SharedDisplayContext> acquire(const Config& config);

    // 通道注册/注销（引用计数）
    int  registerChannel(int channel_id, ChannelLayout layout);
    void unregisterChannel(int channel_id);

    // 通道写入接口（多通道并发安全）
    bool channelWrite(int channel_id, Buffer* decoded_frame);

private:
    // === 设备资源 ===
    int                        fd_;              // /dev/fbX 文件描述符
    int                        screen_width_;
    int                        screen_height_;
    int                        bits_per_pixel_;

    // === Framebuffer 内存（DMA 物理连续）===
    // 通过 taco_sys_get_block 分配，FB_IOCTL_SET_DMA_INFO 设置 DSS DMA 基地址
    struct FbMemory {
        uint32_t blk_id;
        uint64_t phys_addr;
        void*    virt_addr;
        size_t   size;
    };
    FbMemory                   fb_memory_;        // 整块 DMA 内存

    // === BufferPool 管理 4 个 framebuffer 页 ===
    std::unique_ptr<BufferAllocatorFacade> allocator_;
    uint64_t                   fb_pool_id_;       // BufferPool registry ID
    // BufferPool 中的 4 个 Buffer:
    //   Buffer[i].virt_addr = fb_memory_.virt_addr + i * buffer_size
    //   Buffer[i].phys_addr = fb_memory_.phys_addr + i * buffer_size

    // === 渲染/显示状态 ===
    Buffer*                    render_buf_;        // 当前所有通道写入的目标
    Buffer*                    display_buf_;       // 当前 DSS 正在显示的 buffer

    // === 同步原语 ===
    std::shared_mutex          rw_mutex_;          // 读写锁（核心）

    // === 通道布局信息 ===
    struct ChannelInfo {
        int    channel_id;
        int    x, y, w, h;                        // 在 framebuffer 上的区域
        bool   active;
    };
    std::vector<ChannelInfo>   channels_;
    std::mutex                 channel_mgmt_mutex_; // 通道注册/注销保护

    // === 线程 ===
    std::thread                timer_thread_;       // timerfd 定时器线程
    std::thread                display_thread_;     // 显示输出线程
    std::atomic<bool>          running_;
    int                        timer_fd_;           // timerfd 文件描述符
    int                        target_fps_;
};
```

### 3.2 TacoVOFramebufferDevice（继承 LinuxFramebufferDevice）

每个 `DisplayConsumer` 持有一个 `TacoVOFramebufferDevice` 实例，但所有实例共享同一个 `SharedDisplayContext`。

```cpp
class TacoVOFramebufferDevice : public LinuxFramebufferDevice {
public:
    TacoVOFramebufferDevice(const TacoVOConfig& config);
    ~TacoVOFramebufferDevice() override;

    bool initialize(int device_index) override;
    void cleanup() override;

    // displayBuffer 委托给 SharedDisplayContext::channelWrite
    bool displayBuffer(Buffer* buffer) override;

private:
    int                                    channel_id_;
    std::shared_ptr<SharedDisplayContext>   context_;
    TacoVOConfig                           config_;
};
```

---

## 4. 核心机制详解

### 4.1 Framebuffer 内存分配（DMA 物理连续内存）

与原始 `taco-vo` 的 `TA_VO_DEV_IDS` 设备内存分配策略一致：

```
步骤 1: taco_sys_get_block(TACO_INVALID_POOLID, total_size, "shared_display")
         → 获得 blk_id
步骤 2: taco_sys_handle2_phys_addr(blk_id)
         → 获得 phys_addr（物理连续地址）
步骤 3: taco_sys_mmap_noncache(phys_addr, total_size)
         → 获得 virt_addr（用户空间虚拟地址，非缓存映射）
步骤 4: FB_IOCTL_SET_DMA_INFO(fd, {ovl_idx=0, phys_addr})
         → 告知 DSS 驱动：DMA 从此物理地址读取显示数据
```

**为什么不用 mmap(/dev/fbX)?**

| 方式 | 物理地址 | PP 硬件兼容 | DSS DMA |
|------|---------|------------|---------|
| `mmap(/dev/fbX)` | 未知（内核分配） | 不兼容（PP 需要物理地址） | 需要额外配置 |
| `taco_sys_get_block` | 已知（物理连续） | 兼容（直接传物理地址给 PP） | 通过 `FB_IOCTL_SET_DMA_INFO` 配置 |

**内存布局**：

```
fb_memory_.phys_addr
│
├── Buffer[0]: offset = 0 * buffer_size           ← fb_pool_ FREE queue
├── Buffer[1]: offset = 1 * buffer_size           ← fb_pool_ FREE queue
├── Buffer[2]: offset = 2 * buffer_size           ← fb_pool_ FREE queue
└── Buffer[3]: offset = 3 * buffer_size           ← fb_pool_ FREE queue

其中 buffer_size = screen_width * screen_height * bytes_per_pixel
```

每个 `Buffer` 对象拥有 `virt_addr`（CPU 可写）和 `phys_addr`（PP 硬件 / DSS DMA 可用）。

### 4.2 BufferPool 管理 Framebuffer 页

使用组件系统的 `BufferPool` 机制管理 4 个 framebuffer 页：

```
                 BufferPool (fb_pool_)
                 ┌─────────────────┐
                 │   FREE 队列      │ ← 空闲页，可供通道写入
                 │   [Buf2] [Buf3]  │
                 ├─────────────────┤
                 │   FILLED 队列    │ ← 已写完，等待显示
                 │   [Buf0]        │
                 └─────────────────┘
                 
  render_buf_ → Buf1（通道们正在写入的页）
  display_buf_ → Buf0 正在被 DSS 硬件读取显示

  典型稳态下 4 个 buffer 的分配：
  - 1 个：display_buf_（正在显示）
  - 1 个：render_buf_（正在被通道写入）
  - 0~2 个：FILLED 队列（已写完等待显示）
  - 0~2 个：FREE 队列（备用空闲页）
```

**与原 taco-vo 双缓冲的对比**：

| 特性 | 原 taco-vo | 新方案 |
|------|-----------|--------|
| 缓冲页数 | 名义 4 页，实际 2 页交替 | 真正 4 页流水线 |
| 写入和显示解耦 | 不解耦（写完立刻翻页） | 完全解耦（FILLED 队列缓冲） |
| 抗抖动能力 | 无（解码慢则帧率下降） | 有（FILLED 队列可积压 1~2 帧） |

### 4.3 读写锁同步机制（核心）

采用 `std::shared_mutex` 实现通道和定时器的同步：

- **通道写入**：获取 `shared_lock`（共享锁） → 多通道并发
- **定时器切换**：获取 `unique_lock`（独占锁） → 等待所有通道 PP resize 完成

```cpp
// === 通道写入（多通道并发） ===

bool SharedDisplayContext::channelWrite(int channel_id, Buffer* decoded_frame) {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);  // 共享锁

    // 定时器持有独占锁期间，render_buf_ 会被置为 nullptr
    if (render_buf_ == nullptr) {
        return false;  // 定时器正在切换，通知调用方缓存此帧
    }

    // PP 硬件 resize：将解码帧写入 render_buf_ 的对应区域
    // PP 驱动内部线程安全，多通道可并发调用
    ChannelInfo& ch = channels_[channel_id];
    pp_resize(decoded_frame, render_buf_, ch.x, ch.y, ch.w, ch.h);

    return true;
    // shared_lock 析构 → 释放共享锁
}
```

```cpp
// === 定时器线程（周期触发，33ms @ 30fps） ===

void SharedDisplayContext::timerThreadFunc() {
    while (running_) {
        // 阻塞等待 timerfd
        uint64_t expirations;
        read(timer_fd_, &expirations, sizeof(expirations));

        Buffer* old_render;

        // ① 获取独占锁：等待所有正在进行的 PP resize 完成
        {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            old_render = render_buf_;
            render_buf_ = nullptr;  // ② 置 null：新到达的通道直接缓存，不阻塞
        }
        // ③ 立即释放独占锁（持锁时间 = 一次指针赋值 ≈ 微秒级）

        // 以下操作在无锁状态下执行
        // 通道如果此时 acquire shared_lock，会看到 nullptr，return false

        // ④ 尝试获取新的空闲 framebuffer 页
        auto pool = getPool();
        Buffer* new_render = pool->acquireFree(false, 0);  // 非阻塞

        if (!new_render) {
            // 没有空闲 buffer，恢复旧的，跳过本轮
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            render_buf_ = old_render;
            continue;
        }

        // ⑤ 提交旧 buffer 到 FILLED 队列
        pool->submitFilled(old_render);

        // ⑥ PP 硬件 1:1 拷贝：将当前显示内容复制到新页（防止陈旧区域）
        if (display_buf_) {
            pp_copy(display_buf_, new_render);
        }

        // ⑦ 设置新的渲染目标，恢复通道写入
        {
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            render_buf_ = new_render;
        }
        // 通道恢复正常写入
    }
}
```

### 4.4 定时器与通道的时序分析

```
时间轴（33ms 一个周期）：
│←─────────────────── 33ms ──────────────────────→│

通道0: ─[SL ── PP resize(0.8ms) ── unlock]──────────────[SL ── PP ── unlock]─────
通道1: ───[SL ── PP resize(0.6ms) ── unlock]──────────[SL ── PP ── unlock]───────
通道2: ──────[SL ── PP resize(1.0ms) ── unlock]──[SL──nullptr──unlock]──[SL──PP──]

                                                    ↑
                                            定时器 timerfd 到期
                                                    │
                                                    ↓
定时器:                              [UL│等CH2完成│nullptr│UL释放]
                                     │← ~1ms →│  ↑ 微秒级
                                                  │
                                     [无锁: submit → acquireFree → PP_copy]
                                     │←──────── ~500us ─────────→│
                                                                  │
                                     [UL│render_buf_=new│UL释放]
                                      ↑ 微秒级

SL = shared_lock    UL = unique_lock
```

**关键时间指标**：

| 阶段 | 耗时 | 说明 |
|------|------|------|
| 定时器等待独占锁 | 0 ~ 1ms | 等待最慢的正在执行的 PP resize 完成 |
| 独占锁持有时间（第一次） | ~微秒 | 只做一次指针赋值 `render_buf_ = nullptr` |
| 无锁阶段 | ~500us | submit + acquireFree + PP_copy |
| 独占锁持有时间（第二次） | ~微秒 | 只做一次指针赋值 `render_buf_ = new` |
| **总计定时器开销** | **~1.5ms** | 占 33ms 帧周期的 ~4.5%，可接受 |

### 4.5 陈旧区域处理（PP 硬件 1:1 拷贝）

**问题**：当新的 framebuffer 页从 FREE 队列获取时，其内容是旧数据或未初始化数据。如果某个通道本周期没有更新（解码慢），其对应区域会显示垃圾数据。

**方案**：在定时器切换 buffer 时，用 PP 硬件将 `display_buf_`（当前正在显示的页）的完整内容 1:1 拷贝到 `new_render`（新页）。

```
display_buf_ (1920×1080 ARGB)          new_render (1920×1080 ARGB)
┌──────────────────────────┐           ┌──────────────────────────┐
│  CH0(最新)  │  CH1(最新)  │    PP     │  CH0(复制)  │  CH1(复制)  │
│            │            │  ──1:1──→ │            │            │
│  CH2(最新)  │  CH3(最新)  │   拷贝    │  CH2(复制)  │  CH3(复制)  │
└──────────────────────────┘           └──────────────────────────┘
                                        ↑
                                        然后各通道在此基础上写入新数据
                                        未更新的区域保持上一帧内容
```

**使用 `ta_cv_image_resize` 实现 1:1 拷贝**：

```cpp
void pp_copy(Buffer* src, Buffer* dst) {
    ta_cv_resize_image_t resize_attr = {};
    // 不做裁剪，不做缩放
    resize_attr.enable_crop = false;
    resize_attr.enable_scale = false;

    ta_image_t input = {};
    input.width  = screen_width_;
    input.height = screen_height_;
    input.paddr[0] = src->getPhysicalAddress();
    // ... 设置格式等

    ta_image_t output = {};
    output.width  = screen_width_;
    output.height = screen_height_;
    output.paddr[0] = dst->getPhysicalAddress();
    // ... 设置格式等

    ta_cv_image_resize(&resize_attr, input, output);
}
```

**为什么用 PP 硬件而不用 memcpy?**

| 方式 | 速度 | CPU 占用 | 适用场景 |
|------|------|---------|---------|
| `memcpy` | ~2ms (1920×1080×4=8MB) | 高 | 通用，无硬件要求 |
| `ta_cv_image_resize` (PP) | ~0.3ms | 近零（DMA 传输） | 有 PP 硬件的平台 |

### 4.6 显示线程（display thread）

独立线程负责将 FILLED 队列中的 buffer 送到 DSS 硬件显示。

```cpp
void SharedDisplayContext::displayThreadFunc() {
    auto pool = getPool();

    while (running_) {
        // 阻塞获取一个已填充的 buffer
        Buffer* buf = pool->acquireFilled(true, 100);  // 100ms 超时
        if (!buf) continue;

        // 通过 ioctl 切换 DSS 显示到这个 buffer
        struct fb_var_screeninfo var;
        ioctl(fd_, FBIOGET_VSCREENINFO, &var);
        var.yoffset = var.yres * buf->id();
        ioctl(fd_, FBIOPAN_DISPLAY, &var);

        // 等待 VSYNC（确保无撕裂）
        int zero = 0;
        ioctl(fd_, FBIO_WAITFORVSYNC, &zero);

        // 释放上一帧的 display_buf_ → FREE 队列
        Buffer* old_display = display_buf_;
        display_buf_ = buf;

        if (old_display) {
            pool->releaseFilled(old_display);  // → FREE 队列
        }
    }
}
```

**Buffer 生命周期流转**：

```
            acquireFree()              submitFilled()
  FREE ─────────────────→ render_buf_ ──────────────→ FILLED
    ↑                     (通道 PP 写入)               (等待显示)
    │                                                    │
    │                                          acquireFilled()
    │                                                    │
    │           releaseFilled()                           ↓
    └──────────────────────── display_buf_ ←───── FBIOPAN + VSYNC
                              (正在显示)
```

### 4.7 通道写入失败时的帧缓存机制

**场景**：定时器触发时 `render_buf_` 被置为 `nullptr`，此时恰好有通道尝试写入。

**原始行为**（不处理）：`channelWrite()` 返回 `false`，`BufferConsumerService::consumeLoop()` 调用 `pool->releaseFilled(buffer)` 释放该帧 → 帧丢失。

**改进方案**：在 `IBufferConsumer` 接口中增加 `shouldRetainBuffer()` 方法。

#### 4.7.1 接口变更

```cpp
// IBufferConsumer.hpp 新增
class IBufferConsumer {
public:
    // ... 原有接口 ...

    /**
     * 询问消费者是否需要保留当前 buffer（而非归还到 pool）
     * 默认返回 false（兼容现有消费者）
     */
    virtual bool shouldRetainBuffer() const { return false; }
};
```

#### 4.7.2 DisplayConsumer 实现

```cpp
class DisplayConsumer : public IBufferConsumer {
public:
    bool consume(const std::vector<Buffer*>& buffers, int frame_index) override {
        Buffer* decoded = buffers[0];
        bool success = context_->channelWrite(channel_id_, decoded);

        last_consume_failed_ = !success;
        return true;  // 继续消费循环
    }

    bool shouldRetainBuffer() const override {
        return last_consume_failed_;  // 写入失败时请求保留 buffer
    }

private:
    bool last_consume_failed_ = false;
};
```

#### 4.7.3 consumeLoop 改造

```cpp
void BufferConsumerService::consumeLoop(
    std::shared_ptr<BufferPool> pool,
    std::shared_ptr<IBufferConsumer> consumer,
    /* ... */
) {
    Buffer* held_buffer = nullptr;  // 缓存的帧

    while (!stop_requested_) {
        Buffer* buffer = nullptr;

        if (held_buffer) {
            // 上次写入失败，重试缓存的帧
            buffer = held_buffer;
            held_buffer = nullptr;
        } else {
            // 正常获取新帧
            buffer = pool->acquireFilled(true, config.timeout_ms);
            if (!buffer) { /* timeout 处理 */ continue; }
        }

        // 消费
        std::vector<Buffer*> buffers = {buffer};
        consumer->consume(buffers, frame_index);

        // 检查是否需要保留
        if (consumer->shouldRetainBuffer()) {
            held_buffer = buffer;   // 保留，下次重试
        } else {
            pool->releaseFilled(buffer);  // 正常归还
            frame_index++;
            result.frames_consumed++;
        }
    }

    // 清理：如果有未处理的缓存帧，归还
    if (held_buffer) {
        pool->releaseFilled(held_buffer);
    }
}
```

**帧缓存触发频率分析**：

```
定时器 render_buf_ = nullptr 的持续时间 ≈ 500us（acquireFree + PP_copy）
帧周期 = 33ms
触发概率 ≈ 500us / 33ms ≈ 1.5%

即平均每 67 帧才有 1 帧需要缓存重试，且重试只需等待下一个 consume 周期（~微秒级）
```

---

## 5. PP 硬件并发安全性

通过分析 PP 驱动内部实现（`taco_pp_internal.c`），确认以下结论：

### 5.1 PP 驱动内部锁机制

```
taco_pp0_process() / taco_pp1_process()
    ↓
taco_PP0_internal_process_optimized()
    ↓
┌──────────────────────────────────────┐
│  global_mutex (全局锁)                │
│  ↓                                   │
│  pool_mutex (实例池锁)                │
│  → 从 pp_instance_pool 获取空闲实例   │
│  → 获取成功后释放 pool_mutex          │
│  ↓                                   │
│  inst_mutex (实例锁)                  │
│  → 在该实例上执行 PP 操作             │
│  → 完成后归还实例到 pool              │
└──────────────────────────────────────┘
```

### 5.2 结论

| 特性 | 说明 |
|------|------|
| 线程安全 | PP 驱动内部有完整的锁保护，多线程调用安全 |
| 并发能力 | PP 驱动维护实例池，支持多个 PP 操作并发执行 |
| 外部锁需求 | **不需要**为 PP 调用加额外的 mutex |

因此，多个通道可以放心地并发调用 `ta_cv_image_resize`，不需要我们在外层加 PP 的互斥锁。`shared_mutex` 只保护 `render_buf_` 指针的读写一致性。

---

## 6. 通道写入的 PP resize 详解

每个通道将解码帧缩放后写入 `render_buf_` 的指定区域：

```cpp
void SharedDisplayContext::pp_resize(
    Buffer* decoded,         // 输入：解码帧（NV12，有物理地址）
    Buffer* render_buf,      // 输出：当前 framebuffer 页
    int dst_x, int dst_y,   // 目标区域左上角坐标
    int dst_w, int dst_h    // 目标区域宽高
) {
    ta_cv_resize_image_t resize_attr = {};
    resize_attr.enable_crop  = false;
    resize_attr.enable_scale = true;

    // 输入图像（解码帧）
    ta_image_t input = {};
    input.width     = decoded->getImageWidth();
    input.height    = decoded->getImageHeight();
    input.paddr[0]  = decoded->getPhysicalAddress();
    // input.format = NV12 ...

    // 输出图像（framebuffer 的子区域）
    ta_image_t output = {};
    output.width    = dst_w;
    output.height   = dst_h;
    // 计算目标区域的物理地址偏移
    uint64_t base_phys = render_buf->getPhysicalAddress();
    int bytes_per_pixel = bits_per_pixel_ / 8;
    uint64_t offset = (dst_y * screen_width_ + dst_x) * bytes_per_pixel;
    output.paddr[0] = base_phys + offset;
    output.stride   = screen_width_ * bytes_per_pixel;  // 整屏 stride
    // output.format = ARGB888 ...

    ta_cv_image_resize(&resize_attr, input, output);
}
```

**多通道布局示例（3x3 九宫格，1920x1080 屏幕）**：

```
┌─────────┬─────────┬─────────┐
│ CH0     │ CH1     │ CH2     │
│ 640x360 │ 640x360 │ 640x360 │
│(0,0)    │(640,0)  │(1280,0) │
├─────────┼─────────┼─────────┤
│ CH3     │ CH4     │ CH5     │
│ 640x360 │ 640x360 │ 640x360 │
│(0,360)  │(640,360)│(1280,360│
├─────────┼─────────┼─────────┤
│ CH6     │ CH7     │ CH8     │
│ 640x360 │ 640x360 │ 640x360 │
│(0,720)  │(640,720)│(1280,720│
└─────────┴─────────┴─────────┘
```

---

## 7. 生命周期管理

### 7.1 初始化流程

```
用户创建 N 个 VideoProductionLine
    ↓
每个 ProductionLine 的 DisplayConsumer 初始化
    ↓
DisplayConsumer → DisplayDeviceFactory::create(TACO_VO)
    ↓
创建 TacoVOFramebufferDevice
    ↓
TacoVOFramebufferDevice::initialize()
    ↓
SharedDisplayContext::acquire()  ← 第一个调用时创建单例
    ├── 打开 /dev/fbX
    ├── 查询硬件参数（width, height, bpp）
    ├── taco_sys_get_block → 分配 DMA 物理连续内存
    ├── FB_IOCTL_SET_DMA_INFO → 设置 DSS DMA 基地址
    ├── 创建 BufferPool → 注入 4 个 Buffer（携带物理地址）
    ├── 从 FREE 获取第一个 buffer 作为 render_buf_
    ├── 启动定时器线程（timerfd, 30fps）
    └── 启动显示线程
    ↓
SharedDisplayContext::registerChannel(channel_id, layout)
    ├── 计算通道在屏幕上的坐标和尺寸
    └── 标记通道为 active
```

### 7.2 运行时数据流

```
VideoProductionLine::decodeThread()
    ↓ 解码产生 AVFrame
BufferPool(解码).acquireFree() → 填充 → submitFilled()
    ↓
BufferConsumerService::consumeLoop()
    ↓ acquireFilled()
DisplayConsumer::consume(buffer)
    ↓
TacoVOFramebufferDevice::displayBuffer(buffer)
    ↓
SharedDisplayContext::channelWrite(channel_id, buffer)
    ├── shared_lock(rw_mutex_)
    ├── 检查 render_buf_ != nullptr
    ├── PP resize: 解码帧 → render_buf_ 子区域
    └── shared_lock 释放
    ↓
BufferConsumerService: 检查 shouldRetainBuffer()
    ├── false → pool->releaseFilled(buffer)  // 归还解码帧
    └── true  → held_buffer = buffer          // 缓存，下次重试
```

### 7.3 销毁流程

```
用户停止 VideoProductionLine
    ↓
DisplayConsumer::finalize()
    ↓
TacoVOFramebufferDevice::cleanup()
    ↓
SharedDisplayContext::unregisterChannel(channel_id)
    ↓
引用计数减为 0 时 → SharedDisplayContext 析构
    ├── running_ = false
    ├── 等待定时器线程退出
    ├── 等待显示线程退出
    ├── 关闭 timerfd
    ├── 销毁 BufferPool
    ├── taco_sys_munmap / taco_sys_release_block → 释放 DMA 内存
    └── close(fd_)
```

---

## 8. 线程模型总览

| 线程 | 数量 | 职责 | 锁行为 |
|------|------|------|--------|
| 通道解码线程 | N（每个 VideoProductionLine 一个） | 解码 → 填充 BufferPool | 无（各自 BufferPool） |
| 通道消费线程 | N（每个 consumeLoop 一个） | acquireFilled → channelWrite | `shared_lock(rw_mutex_)` |
| 定时器线程 | 1 | timerfd → 切换 render_buf_ | `unique_lock(rw_mutex_)` |
| 显示线程 | 1 | acquireFilled → FBIOPAN → VSYNC | 无（通过 BufferPool 队列同步） |

**锁竞争分析**：

```
                        shared_lock              unique_lock
                        (通道消费线程)            (定时器线程)
                        
  可并发？                  是（N 个通道并发）       否（独占）
  持锁时间                  0.5~1ms (PP resize)     ~微秒（指针赋值）
  频率                      每帧一次                每 33ms 一次
  阻塞等待                  仅在定时器持独占锁时      等所有 shared_lock 释放
  阻塞时长                  ~微秒                   0~1ms
```

---

## 9. 文件变更清单（路径已随架构迁移更新）

> 本文档撰写时的路径为 `include/display/` 等；当前代码中显示实现与工厂已归 **`include/vendor/taco/display/`**、**`source/vendor/taco/display/`**，显示接口契约在 **`include/vendor/contracts/IDisplayDevice.hpp`**，消费侧在 **`consumptionline/`**。下表保留原设计意图说明，路径以仓库现状为准。

| 文件（现行路径） | 变更类型 | 说明 |
|------|---------|------|
| `components/include/vendor/taco/display/SharedDisplayContext.hpp` | 实现 | 共享显示上下文单例 |
| `components/source/vendor/taco/display/SharedDisplayContext.cpp` | 实现 | DMA 分配、BufferPool、timerfd、显示线程 |
| `components/include/vendor/taco/display/SharedFramebufferDevice.hpp` | 实现 | `IDisplayDevice` 实现，委托 `SharedDisplayContext` |
| `components/source/vendor/taco/display/SharedFramebufferDevice.cpp` | 实现 | 同上 |
| `components/include/vendor/contracts/IDisplayDevice.hpp` | 契约 | 显示设备抽象 |
| `components/include/consumptionline/IBufferConsumer.hpp` | 实现 | 含 `shouldRetainBuffer()` 等 |
| `components/source/consumptionline/BufferConsumerStrategies.cpp` | 实现 | DisplayConsumer 等 |
| `components/source/consumptionline/BufferConsumerService.cpp` | 实现 | consumeLoop 等 |
| `components/source/vendor/taco/display/DisplayDeviceFactory.cpp` | 实现 | 创建 Taco VO / Shared FB 等设备 |

---

## 10. 与原 taco-vo 功能对照表

| 原 taco-vo 功能 | 新方案实现 | 位置 |
|----------------|-----------|------|
| `ta_vo_create_dev` | `SharedDisplayContext::acquire()` 中 open /dev/fbX | SharedDisplayContext.cpp |
| `ta_vo_create_layer` | 由 DSS sysfs 配置替代（overlay enable, pixel format） | SharedDisplayContext.cpp |
| `ta_vo_create_channel` | `SharedDisplayContext::registerChannel()` | SharedDisplayContext.cpp |
| `ta_vo_chn_send_frame` | `SharedDisplayContext::channelWrite()` (PP resize) | SharedDisplayContext.cpp |
| `ta_vo_destroy_channel` | `SharedDisplayContext::unregisterChannel()` | SharedDisplayContext.cpp |
| `ta_vo_destroy_layer/dev` | `SharedDisplayContext` 析构 | SharedDisplayContext.cpp |
| DMA 内存分配 (`taco_sys_get_block`) | 保留，在 `SharedDisplayContext` 初始化时调用 | SharedDisplayContext.cpp |
| `FB_IOCTL_SET_DMA_INFO` | 保留，在初始化时设置 DSS DMA 基地址 | SharedDisplayContext.cpp |
| 帧发送 (`memcpy` → DMA buffer) | 替换为 PP 硬件 resize（零 CPU 拷贝） | SharedDisplayContext.cpp |
| 前后 buffer 交替 | 替换为 BufferPool FREE/FILLED 队列 | SharedDisplayContext.cpp |
| VSYNC 等待 | 保留在显示线程中 | SharedDisplayContext.cpp |

---

## 11. 关键设计决策总结

| 决策项 | 选择 | 理由 |
|--------|------|------|
| Framebuffer 内存分配 | `taco_sys_get_block`（非 mmap /dev/fbX） | PP 硬件需要物理地址，DMA 需要物理连续内存 |
| 多缓冲管理 | 组件系统 `BufferPool` | 复用现有基础设施，FREE/FILLED 队列天然适合生产者-消费者模型 |
| 通道同步 | `std::shared_mutex`（读写锁） | 通道并发（shared），定时器独占（unique），PP 保证完成 |
| 定时器实现 | `timerfd`（Linux 精确定时） | 无漂移、低 CPU、与 epoll 兼容 |
| 陈旧区域处理 | PP 硬件 1:1 拷贝（`ta_cv_image_resize`） | 比 CPU memcpy 快 ~6x，近零 CPU 占用 |
| 帧丢失处理 | `shouldRetainBuffer()` + held_buffer 缓存 | 零帧丢失，对现有接口改动最小 |
| PP 互斥锁 | 不需要 | PP 驱动内部有完整锁机制和实例池，线程安全 |
| 定时器是否等通道 | 等（通过 unique_lock） | 保证 PP resize 一定完成，消除竞态 |
| render_buf_ 类型 | 普通指针（`Buffer*`） | 所有访问都在 shared_mutex 保护下，不需要 atomic |
