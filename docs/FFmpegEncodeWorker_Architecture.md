# FFmpegEncodeWorker 技术架构文档

## 1. 概述

`FFmpegEncodeWorker` 是一个基于 FFmpeg 的视频编码 Worker，负责将原始帧（YUV/RGB `AVFrame`）编码为压缩数据（H.264/H.265/JPEG `AVPacket`）。它继承自 `WorkerBase`，可集成到 `MultiWorkerProductionLine` 中作为消费者，也可独立运行。

**文件位置：**
- 头文件：`components/include/productionline/worker/core/FFmpegEncodeWorker.hpp`
- 实现文件：`components/source/productionline/worker/core/FFmpegEncodeWorker.cpp`

---

## 2. 类继承关系

```
WorkerBase (抽象基类)
  ├── 实现 IDataSourceNavigator (导航接口: open/close/seek/getTotalFrames...)
  │
  ├── FFmpegDecodeWorker (解码: AVPacket → AVFrame)
  │
  └── FFmpegEncodeWorker (编码: AVFrame → AVPacket)  ◀── 本文档
          │
          └── 使用 IRawFrameSource 获取输入帧
              ├── RawFrameSourceFromFile   (文件模式: 从 YUV 文件读取)
              └── RawFrameSourceFromBuffer (Buffer 模式: 从上游 BufferPool 获取)
```

---

## 3. 整体架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                      FFmpegEncodeWorker                              │
│                                                                     │
│  ┌─────────────────── 输入层 ───────────────────────────────────┐   │
│  │                 IRawFrameSource                               │   │
│  │                                                               │   │
│  │  ┌─ RawFrameSourceFromBuffer ─┐  ┌─ RawFrameSourceFromFile ─┐│   │
│  │  │ setSourceBufferPool()      │  │ open(path)               ││   │
│  │  │ pool→waitForFilled()       │  │ readRawFrame(input_frame)││   │
│  │  │ Buffer→getAVFrame()        │  │ (读取 YUV 像素数据)      ││   │
│  │  └─────────────┬──────────────┘  └────────────┬─────────────┘│   │
│  └────────────────┼───────────────────────────────┼──────────────┘   │
│                   │                               │                  │
│                   ▼                               ▼                  │
│  ┌─────────────── readAndSendFrame() ───────────────────────────┐   │
│  │                                                               │   │
│  │  1. frame_source_->readRawFrame(temp_frame)                   │   │
│  │  2. [可选] buf_src->getDirectFrame() → 直接模式帧             │   │
│  │  3. [可选] sws_scale() → 缩放到编码分辨率                     │   │
│  │  4. encode_frame->pts = encoded_frames_.load()                │   │
│  │  5. avcodec_send_frame(codec_ctx_, encode_frame)              │   │
│  │                                                               │   │
│  │  返回: FillResult (success/eof/eagain/error)                  │   │
│  └───────────────────────────┬───────────────────────────────────┘   │
│                              ▼                                       │
│  ┌─────────────── fillBuffer() 核心编码循环 ────────────────────┐   │
│  │                                                               │   │
│  │  步骤1: 检查 cached_packets_ 缓存                            │   │
│  │         ├── 有 → av_packet_move_ref → 直接返回 Success        │   │
│  │         └── 无 → 进入步骤2                                    │   │
│  │                                                               │   │
│  │  步骤2: readAndSendFrame()                                    │   │
│  │         ├── 文件模式 → 复用 input_frame_                      │   │
│  │         └── Buffer模式 → av_frame_alloc() 临时帧              │   │
│  │                                                               │   │
│  │  步骤3: avcodec_receive_packet() 循环                         │   │
│  │         ├── ret == 0     → 成功, push_back 到 cached_packets_ │   │
│  │         ├── ret == EAGAIN → 编码器需要更多帧, break            │   │
│  │         ├── ret == EOF    → flush 完成, break                  │   │
│  │         └── 其他          → 错误, break                        │   │
│  │                                                               │   │
│  │  步骤4: 从 cached_packets_ 取首个 packet 填充 buffer          │   │
│  │         └── 首帧: syncOutputCodecParameters() 更新 SPS/PPS    │   │
│  └───────────────────────────┬───────────────────────────────────┘   │
│                              ▼                                       │
│  ┌─────────────── 输出层 ───────────────────────────────────────┐   │
│  │  fillBufferMetadataFromPacket()                               │   │
│  │    - buffer->setSize(packet->size)                            │   │
│  │    - buffer->setVirtualAddress(packet->data)                  │   │
│  │    - encoded_frames_++                                        │   │
│  └───────────────────────────┬───────────────────────────────────┘   │
│                              ▼                                       │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  BufferPool (ENCODE_VIDEO_OUTPUT)                             │   │
│  │  → 下游 BufferWriter / Muxer / EncodedPacketSourceFromBuffer │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 4. 核心函数详解

### 4.1 构造函数

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:23
FFmpegEncodeWorker::FFmpegEncodeWorker(const WorkerConfig& config)
    : WorkerBase(BufferAllocatorFactory::AllocatorType::AVFRAME, config)
```

构造函数根据 `WorkerConfig` 中的数据源配置，创建不同类型的 `IRawFrameSource`：

| 条件 | 创建的 Source | 说明 |
|------|-------------|------|
| `config.data_source.shared_raw_frame_source` 非空 | 使用外部注入的 source | 直接模式（如管线内传递） |
| `config.data_source.buffer_mode == true` | `RawFrameSourceFromBuffer` | Buffer 模式：从解码器 BufferPool 获取帧 |
| `config.data_source.path` 非空 | `RawFrameSourceFromFile` | 文件模式：从 YUV 文件读取原始帧 |
| 以上均不满足 | 无（延后通过 `setSourceBufferPool` 设置） | 等待外部注入 |

此外，构造函数会检查源分辨率与编码分辨率是否一致，若不一致则设置 `input_scale_needed_ = true`。

### 4.2 open() — 初始化编码器

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:122
bool FFmpegEncodeWorker::open();
```

**执行流程（共 7 步）：**

```
open()
  │
  ├─ 1. frame_source_->open()              // 打开帧数据源
  │
  ├─ 2. 确定 output_width_ / output_height_  // 如为 0 则从 source 获取
  │
  ├─ 3. initializeEncoder()                 // 查找并打开 FFmpeg 编码器
  │     ├─ avcodec_find_encoder_by_name()   // 按名称查找
  │     ├─ avcodec_alloc_context3()         // 分配上下文
  │     ├─ 设置参数: width/height/bitrate/gop/framerate/pix_fmt
  │     ├─ 码率控制: CBR(0)/VBR(1)/CQP(2)
  │     ├─ TACO 特定: configureTacoEncoder()
  │     ├─ JPEG 特定: quality + pix_fmt 调整
  │     └─ avcodec_open2()                  // 打开编码器
  │
  ├─ 3.1 syncOutputCodecParameters()        // 生成 AVCodecParameters
  │       └─ avcodec_parameters_from_context()
  │
  ├─ 4. allocator_facade_.allocatePoolWithBuffers()  // 创建输出 BufferPool
  │
  ├─ 5. registerBufferPool(ENCODE_VIDEO_OUTPUT, pool_id)
  │
  ├─ 6. [文件模式] 分配 input_frame_ 壳子
  │     └─ [需要缩放] 创建 scaled_frame_ + sws_getContext()
  │
  └─ 7. 日志输出: Codec / Resolution / Bitrate / GOP / Framerate / BufferPool
```

**关键设计决策：**
- **文件模式复用 `input_frame_`**：不在每帧 `readRawFrame` 时 `av_frame_alloc`，而是复用同一帧结构体。`readRawFrame` 内部在首次调用时 Lazy 分配 buffer（`av_frame_get_buffer`）。这避免了嵌入式设备上的瞬时内存峰值和 OOM。
- **`codec_params_extradata_ready_`**：硬件编码器（如 `h264_taco`）可能在 `avcodec_open2` 后 `extradata` 仍为空，需要等到首个 packet 产出后才填充 SPS/PPS。此标志位控制何时让下游解码器获取完整参数。

### 4.3 fillBuffer() — 编码一帧（核心方法）

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:866
FillResult FFmpegEncodeWorker::fillBuffer(int frame_index, Buffer* buffer) override;
```

这是 `WorkerBase` 调度框架的核心回调，每次从 free queue 获取到一个 `Buffer` 时被调用。

**状态转换图：**

```
                    fillBuffer(buffer)
                         │
                         ▼
              ┌─ cached_packets_ 非空? ─┐
              │                          │
         Yes  ▼                     No   ▼
  ┌───────────────────┐    ┌─────────────────────────┐
  │ 取首个 cached_pkt │    │ readAndSendFrame()      │
  │ av_packet_move_ref│    │  → 获取帧 + 送入编码器  │
  │ → buffer          │    └────────────┬────────────┘
  │                   │                 │
  │ fillBuffer        │                 ▼
  │ MetadataFromPacket│    ┌─────────────────────────┐
  │                   │    │ avcodec_receive_packet() │
  │ return Success    │    │ 循环直到 EAGAIN/EOF      │
  └───────────────────┘    │ 每个 packet → cached_    │
                           │ packets_.push_back()     │
                           └────────────┬────────────┘
                                        │
                                        ▼
                           ┌─ cached_packets_ 非空? ─┐
                           │                          │
                      Yes  ▼                     No   ▼
               ┌──────────────────┐    ┌──────────────────┐
               │ 取首个 → buffer  │    │ 无 packet 产出   │
               │ fillMetadata     │    │ EAGAIN → dropped_ │
               │ syncCodecParams  │    │ frames_++         │
               │ (首帧: SPS/PPS) │    │ return fromCodec  │
               │ return Success   │    │ (receive_result)  │
               └──────────────────┘    └──────────────────┘
```

**`cached_packets_` 机制的设计原理：**

一帧输入可能产生多个输出 packet（如 B 帧场景下编码器内部缓存后一次输出多帧），但 `fillBuffer` 每次只能填充一个 `Buffer`。因此：
1. 将所有 `avcodec_receive_packet` 成功的 packet 存入 `cached_packets_`
2. 取第一个填充到当前 buffer
3. 后续 `fillBuffer` 调用优先从缓存取，无需再次 `readAndSendFrame`

### 4.4 readAndSendFrame() — 读取并发送帧

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:774
FillResult FFmpegEncodeWorker::readAndSendFrame(AVFrame* temp_frame);
```

**详细流程：**

```
readAndSendFrame(temp_frame)
  │
  ├─ 1. frame_source_->readRawFrame(temp_frame)
  │     ├─ ret < 0 且 AVERROR_EOF → return FillResult::fromAcquire(eof)
  │     ├─ ret < 0 且 AVERROR(EAGAIN) → return fromAcquire(again)
  │     └─ ret < 0 其他 → return fromAcquire(unknownError)
  │
  ├─ 2. 确定实际编码帧 (encode_frame)
  │     ├─ 默认: encode_frame = temp_frame
  │     └─ 直接模式: buf_src->getDirectFrame() → 如非 null 则替代
  │
  ├─ 3. 设置 PTS
  │     └─ encode_frame->pts = encoded_frames_.load()
  │
  ├─ 4. [需要缩放] sws_scale() → scaled_frame_
  │     └─ scaled_frame_->pts = encoded_frames_
  │
  └─ 5. avcodec_send_frame(codec_ctx_, frame)
        ├─ ret == 0     → return FillResult::success()
        ├─ ret == EOF   → fromCodec(eof)
        ├─ ret == EAGAIN → fromCodec(eagain)
        ├─ ret == EINVAL → fromCodec(invalidState)
        ├─ ret == ENOMEM → fromCodec(allocFailed)
        └─ 其他          → fromCodec(encodeError)
```

**`FillResult` 类型说明：**

`FillResult` 是 v2.33 重构引入的类型安全返回值，替代了原始的 `bool`。它区分了三个阶段的错误来源：

| 阶段 | 工厂方法 | 含义 |
|------|---------|------|
| Acquire（获取帧） | `FillResult::fromAcquire(PacketAcquireResult)` | 帧获取失败（EOF/EAGAIN/Error） |
| Codec（编码器交互） | `FillResult::fromCodec(CodecSendResult)` | 编码器返回异常 |
| 通用 | `FillResult::success()` / `invalidParam()` / `notOpen()` | 参数校验或状态错误 |

### 4.5 syncOutputCodecParameters() — 同步输出参数

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:730
bool FFmpegEncodeWorker::syncOutputCodecParameters();
```

将编码器上下文 (`codec_ctx_ptr_`) 的参数导出为 `AVCodecParameters`，供下游组件（如 `EncodedPacketSourceFromBuffer`）通过 `getCodecParameters()` 获取。

**关键约束：** 不可重复 `free/realloc out_codec_params_`，因为 `MultiWorkerProductionLine` 中的解码侧可能持有该指针引用，重新分配会导致悬垂指针。

**调用时机：**
1. `open()` 中初始化后立即调用（此时 `extradata` 可能为空）
2. `fillBuffer()` 中首个 packet 产出后再次调用（此时 `extradata` 已填充 SPS/PPS）

### 4.6 close() — 关闭编码器

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:343
void FFmpegEncodeWorker::close();
```

**清理流程（严格顺序）：**

```
close()
  │
  ├─ 1. 释放 scaled_frame_ / sws_ctx_ / input_frame_
  │
  ├─ 2. 清理 cached_packets_（av_packet_free 每个缓存 packet）
  │
  ├─ 3. [Buffer模式] commitRawFrame() 归还当前帧
  │
  ├─ 4. 刷新编码器 (Drain)
  │     ├─ avcodec_send_frame(nullptr)  // flush 信号
  │     └─ 循环 avcodec_receive_packet() 丢弃剩余 packet
  │
  ├─ 5. frame_source_->close()
  │
  ├─ 6. freeOutputCodecParameters()
  │
  ├─ 7. avcodec_free_context(&codec_ctx_ptr_)
  │
  ├─ 8. av_dict_free(&codec_options_ptr_)
  │
  └─ 9. clearAllBufferPools()
```

**编码器 Drain 机制：**

向编码器发送 `nullptr` 帧触发 flush 模式（draining），然后循环接收所有剩余 packet 并丢弃。这确保编码器内部缓存的 B 帧等被完整输出。`AVERROR_EOF` 在 flush 中的 `send_frame(nullptr)` 中是正常的（表示已在 draining），不视为错误。

### 4.7 configureTacoEncoder() — TACO 硬件编码器配置

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:755
bool FFmpegEncodeWorker::configureTacoEncoder();
```

通过 `WorkerConfig::encoder.vendor` 提供的 `VendorEncoderExtension` 接口，将 TACO 芯片特定的编码参数应用到 `codec_ctx_->priv_data`。

### 4.8 fillBufferMetadataFromPacket() — 填充输出 Buffer 元数据

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:842
bool FFmpegEncodeWorker::fillBufferMetadataFromPacket(AVPacket* packet, Buffer* buffer);
```

将编码完成的 `AVPacket` 的元数据写入输出 `Buffer`：
- `buffer->setSize(packet->size)` — 设置压缩数据大小
- `buffer->setVirtualAddress(packet->data)` — 设置数据指针
- `encoded_frames_++` — 递增计数

---

## 5. 成员变量详解

### 5.1 核心编码器状态

| 变量 | 类型 | 作用 |
|------|------|------|
| `codec_ctx_ptr_` | `AVCodecContext*` | FFmpeg 编码器上下文 |
| `codec_options_ptr_` | `AVDictionary*` | 编码器选项字典（`rc-mode`, `qp`, `quality` 等） |
| `out_codec_params_` | `AVCodecParameters*` | 导出的编码参数（供下游 `getCodecParameters()` 访问） |
| `codec_params_extradata_ready_` | `bool` | SPS/PPS(extradata) 是否已就绪 |

### 5.2 输入帧管理

| 变量 | 类型 | 作用 |
|------|------|------|
| `frame_source_` | `shared_ptr<IRawFrameSource>` | 帧数据源抽象 |
| `input_frame_` | `AVFrame*` | 文件模式复用帧（Lazy alloc buffer） |
| `current_frame_ptr_` | `AVFrame*` | Buffer 模式当前帧指针 |
| `frame_acquired_` | `bool` | Buffer 模式帧获取状态 |

### 5.3 缩放支持

| 变量 | 类型 | 作用 |
|------|------|------|
| `input_scale_needed_` | `bool` | 源分辨率≠编码分辨率时为 true |
| `sws_ctx_` | `SwsContext*` | libswscale 缩放上下文 |
| `scaled_frame_` | `AVFrame*` | 缩放后的帧（尺寸 = output_width_ × output_height_） |

### 5.4 输出缓存与统计

| 变量 | 类型 | 作用 |
|------|------|------|
| `cached_packets_` | `vector<AVPacket*>` | 编码器一次输出多个 packet 时的缓冲区 |
| `encoded_frames_` | `atomic<int>` | 成功编码帧计数 |
| `dropped_frames_` | `atomic<int>` | 因 EAGAIN 而丢弃的帧计数 |
| `mutex_` | `recursive_mutex` | 保护编码器状态的线程锁 |

---

## 6. 支持的编码器与配置

| 编码器名称 | 类型 | 编解码标准 | 特殊配置 |
|-----------|------|-----------|---------|
| `h264_taco` | 硬件 | H.264/AVC | `rc-mode` 支持 `cbr`/`vbr`/`2`(CQP)；CQP 时 `bit_rate` 最低 4Mbps 占位 |
| `hevc_taco` | 硬件 | H.265/HEVC | 同上 |
| `jpeg_taco` | 硬件 | JPEG | `quality` 参数；直接接受 NV12 输入 |
| `libx264` | 软件 | H.264/AVC | `rc-mode` 支持 `cbr`/`vbr` |
| `libx265` | 软件 | H.265/HEVC | 同上 |
| `mjpeg` | 软件 | MJPEG | `quality` 参数；NV12/YUV420P 自动转 YUVJ420P |

**码率控制模式映射：**

```
WorkerConfig::encoder.rc_mode:
  0 → CBR  → av_dict_set("rc-mode", "cbr")
  1 → VBR  → av_dict_set("rc-mode", "vbr")
  2 → CQP  → av_dict_set("rc-mode", "2")     // 仅 TACO
                + av_dict_set("qp", "<1-51>")
```

> **注意：** TACO 硬件编码器的 `rc-mode` 使用表达式求值，已注册符号为 `cbr`/`vbr`，但 CQP 模式需要用整型字符串 `"2"`，不能用 `"cqp"`（会报 `Undefined constant 'cqp'`）。

---

## 7. 与 MultiWorkerProductionLine 的集成

### 7.1 转码管线

```
[输入文件 .h264/.hevc]
       │
       ▼
[FFmpegDecodeWorker]  ←── RecordWorker 读取文件并提供 EncodedPacket
       │
       ▼
  BufferPool (DECODE_VIDEO_PRIMARY)
       │
       ▼
[FFmpegEncodeWorker]  ←── setSourceBufferPool() 连接上游
       │
       ▼
  BufferPool (ENCODE_VIDEO_OUTPUT)
       │
       ▼
[BufferWriter]  ←── 写入输出文件
```

### 7.2 COMPARE 模式（硬件 vs 软件）

```
                       EncodedPacketSourceFromBuffer
                              │           │
                              ▼           ▼
                     [HW Decoder]  [SW Decoder]
                              │           │
                              ▼           ▼
                     BufferPool_HW  BufferPool_SW
                              │           │
                              └─────┬─────┘
                                    ▼
                        WorkerSyncCoordinator
                           arrive() → 比较
```

在 COMPARE 模式下，`FFmpegEncodeWorker` 不直接参与，但其产出的 `AVCodecParameters`（通过 `getCodecParameters()`）被 `EncodedPacketSourceFromBuffer` 用于初始化下游解码器。

### 7.3 setSourceBufferPool 的连接机制

```cpp
// 文件: components/source/productionline/worker/core/FFmpegEncodeWorker.cpp:548
bool FFmpegEncodeWorker::setSourceBufferPool(std::weak_ptr<BufferPool> pool_weak) {
    auto* buffer_source = dynamic_cast<RawFrameSourceFromBuffer*>(frame_source_.get());
    buffer_source->setSourceBufferPool(pool_weak);
}
```

该方法将上游解码器的 `BufferPool` 连接到编码器的 `RawFrameSourceFromBuffer`。当 `fillBuffer()` 被调用时，`readAndSendFrame()` → `frame_source_->readRawFrame()` → `RawFrameSourceFromBuffer::readRawFrame()` → `pool_->waitForFilled()` 从上游获取帧。

---

## 8. 线程安全设计

| 保护对象 | 机制 | 说明 |
|---------|------|------|
| 编码器状态（open/close/fillBuffer） | `recursive_mutex mutex_` | 允许同一线程重入（如 close 中调用 fillBuffer 相关逻辑） |
| `encoded_frames_` / `dropped_frames_` | `std::atomic<int>` | 无锁计数，支持跨线程读取 |
| `cached_packets_` | `mutex_` 保护 | 仅在 `fillBuffer()` 中操作（由调度框架保证单线程调用） |
| `out_codec_params_` | 写后只读 | `syncOutputCodecParameters()` 后不再修改，安全供下游读取 |

---

## 9. 内存管理（资源生命周期）

| 资源 | 创建时机 | 创建方法 | 释放时机 | 释放方法 |
|------|---------|---------|---------|---------|
| `codec_ctx_ptr_` | `initializeEncoder()` | `avcodec_alloc_context3()` | `close()` | `avcodec_free_context()` |
| `out_codec_params_` | `syncOutputCodecParameters()` | `avcodec_parameters_alloc()` | `close()` | `avcodec_parameters_free()` |
| `input_frame_` | `open()` (文件模式) | `av_frame_alloc()` | `close()` / 析构 | `av_frame_free()` |
| `scaled_frame_` | `open()` (需要缩放时) | `av_frame_alloc()` + `av_frame_get_buffer()` | `close()` | `av_frame_free()` |
| `sws_ctx_` | `open()` (需要缩放时) | `sws_getContext()` | `close()` | `sws_freeContext()` |
| `cached_packets_[i]` | `fillBuffer()` 循环 | `av_packet_alloc()` | 被消费时 / `close()` / 析构 | `av_packet_free()` |
| `codec_options_ptr_` | `initializeEncoder()` | `av_dict_set()` | `close()` | `av_dict_free()` |

**析构函数的清理顺序：**
1. 清理 `cached_packets_`
2. 手动清理 `BufferPool`（`allocator_facade_.destroyPool()` + `clearAllBufferPools()`）
3. 关闭编码器和数据源（`close()`）
4. 如果 `close()` 未被调用但 `input_frame_` 存在，单独释放

---

## 10. 关键设计原则

1. **帧复用避免 OOM**：文件模式下 `input_frame_` 只分配一次壳子，内部 buffer 通过 `readRawFrame` Lazy 分配。每帧编码完成后 `av_frame_unref(input_frame_)` 释放像素数据但保留结构体。

2. **延迟 extradata 同步**：`codec_params_extradata_ready_` 标志确保下游解码器在硬件编码器首帧产出后才获取完整的 SPS/PPS 参数。

3. **Drain 安全关闭**：`close()` 通过发送 `nullptr` 帧触发编码器 drain，确保内部缓存的帧被完整输出，避免资源泄漏。

4. **FillResult 类型安全**：v2.33 引入的 `FillResult` 替代 `bool` 返回值，精确区分 Acquire/Codec/通用 三个阶段的错误来源，便于上层调度框架做出正确的重试或终止决策。

5. **缓存多 packet 输出**：`cached_packets_` 机制处理一帧输入产生多帧输出的场景（如 B 帧编码器延迟输出），保证每个 packet 都被正确传递到输出 BufferPool。
