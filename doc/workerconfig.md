# WorkerConfig 开发者文档

> **版本**: v2.24  
> **更新日期**: 2026-01-29  
> **目标读者**: 使用 Components 库的开发者

---

## 目录

- [概述](#概述)
- [设计理念](#设计理念)
- [快速开始](#快速开始)
- [枚举类型](#枚举类型)
- [配置结构体](#配置结构体)
- [Builder 类](#builder-类)
- [MultiWorker 配置](#multiworker-配置)
- [完整示例](#完整示例)

---

## 概述

`WorkerConfig` 是 Components 库的核心配置系统，提供了一套完整的 **Builder 模式** API，用于配置视频解码、录制等 Worker 的各项参数。

### 主要特性

- ✅ **Builder 模式**：链式调用，易用易读
- ✅ **类型安全**：使用枚举类型代替魔法数字
- ✅ **配置分离**：数据源、显示设备、解码器配置独立
- ✅ **职责清晰**：每个 Builder 只负责自己层级的配置
- ✅ **向后兼容**：保留旧接口，支持平滑迁移

---

## 设计理念

### 配置结构层次

```
WorkerConfig (顶层配置)
├── DataSourceConfig (数据源配置)
│   ├── 基础配置 (path, buffer_count)
│   └── 数据源模式 (buffer_mode, codec_params, shared_packet_source)
├── DisplayConfig (显示设备配置)
│   └── 分辨率和像素格式
├── DecoderConfig (解码器配置)
│   ├── 通用参数 (name, enable_hardware, hwaccel_device)
│   └── TacoConfig (TACO 解码器特定配置)
│       ├── 通道0配置 (YUV 输出)
│       └── 通道1配置 (RGB/YUV 输出)
└── worker_type (Worker 类型)
```

### Builder 模式优势

```cpp
// ❌ 旧方式：直接构造，参数众多，易出错
WorkerConfig config;
config.data_source.path = "rtsp://...";
config.data_source.buffer_count = 8;
config.decoder.name = "h264_taco";
config.decoder.taco.ch0_enable = true;
// ... 数十行配置代码

// ✅ 新方式：Builder 模式，清晰易读
auto config = WorkerConfigBuilder()
    .setDataSourceConfig(
        DataSourceConfigBuilder()
            .setPath("rtsp://...")
            .setBufferCount(8)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useTaco("h264", 
                TacoConfigBuilder()
                    .setChannels(true, false)
                    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12)
                    .build()
            )
            .build()
    )
    .build();
```

---

## 快速开始

### 示例 1：RTSP 流解码（TACO 硬件解码）

```cpp
#include "productionline/worker/WorkerConfig.hpp"

// 配置数据源
auto dataSource = DataSourceConfigBuilder()
    .setPath("rtsp://192.168.1.100/stream")
    .setBufferCount(8)
    .build();

// 配置 TACO 解码器
auto tacoConfig = TacoConfigBuilder()
    .setChannels(true, false)  // 启用通道0，禁用通道1
    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT709)
    .setScale(Channel::CH0, 1920, 1080)
    .build();

auto decoder = DecoderConfigBuilder()
    .useTaco("h264", tacoConfig)
    .build();

// 组装完整配置
auto config = WorkerConfigBuilder()
    .setDataSourceConfig(dataSource)
    .setDecoderConfig(decoder)
    .setWorkerType(WorkerType::FFMPEG_DECODE)
    .build();
```

### 示例 2：本地文件解码（软件解码）

```cpp
auto config = WorkerConfigBuilder()
    .setDataSourceConfig(
        DataSourceConfigBuilder()
            .setPath("/data/video.mp4")
            .setBufferCount(128)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useSoftware()
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_DECODE)
    .build();
```

---

## 枚举类型

### WorkerType

Worker 实现类型枚举。

| 枚举值 | 含义 | 说明 |
|--------|------|------|
| `AUTO` | 自动检测 | 根据数据源路径自动选择 Worker 类型 |
| `FFMPEG_DECODE` | FFmpeg 解码 Worker | 统一处理文件和 RTSP 流解码 |
| `FFMPEG_PACKET_RECORDER` | FFmpeg Packet 录制器 | 支持 RTSP/文件/HTTP 等多种数据源录制 |

**使用示例**：
```cpp
WorkerConfigBuilder().setWorkerType(WorkerType::FFMPEG_DECODE).build();
```

---

### Channel

TACO 解码器输出通道枚举。

| 枚举值 | 含义 | 支持格式 |
|--------|------|----------|
| `CH0` | 通道0 | 仅支持 YUV 格式输出 |
| `CH1` | 通道1 | 支持 RGB 和 YUV 格式输出 |

**使用示例**：
```cpp
TacoConfigBuilder()
    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12)
    .setScale(Channel::CH1, 1920, 1080)
    .build();
```

---

### OutputFormat

视频输出格式枚举（包含 YUV 和 RGB 格式）。

#### YUV 格式（通道0和通道1都支持）

| 枚举值 | 含义 | 说明 |
|--------|------|------|
| `YUV_AUTO` | 自动选择 YUV 格式 | 由解码器根据输入流决定 |
| `YUV_NV12` | NV12 | YUV420 semi-planar, UV interleaved |
| `YUV_NV21` | NV21 | YUV420 semi-planar, VU interleaved |
| `YUV_I420` | I420/YUV420P | YUV420 planar |
| `YUV_YV12` | YV12 | YUV420 planar, V before U |
| `YUV_P010` | P010 | 10-bit YUV420 semi-planar |
| `YUV_NV16` | NV16 | YUV422 semi-planar |
| `YUV_NV61` | NV61 | YUV422 semi-planar, VU interleaved |
| `YUV_I422` | I422 | YUV422 planar |
| `YUV_NV24` | NV24 | YUV444 semi-planar |
| `YUV_I444` | I444 | YUV444 planar |

#### RGB 格式（仅通道1支持）

| 枚举值 | 含义 | 说明 |
|--------|------|------|
| `RGB_ARGB888` | ARGB8888 packed | 驱动值: 9 |
| `RGB_ABGR888` | ABGR8888 packed | 驱动值: 11 |
| `RGB_RGBA888` | RGBA8888 packed | 驱动值: 13 |
| `RGB_BGRA888` | BGRA8888 packed | 驱动值: 15 |
| `RGB_RGB888` | RGB888 packed | 驱动值: 1 |
| `RGB_BGR888` | BGR888 packed | 驱动值: 3 |
| `RGB_XRGB888` | XRGB8888 packed | 驱动值: 25 |
| `RGB_XBGR888` | XBGR8888 packed | 驱动值: 27 |
| `RGB_RGBX888` | RGBX8888 packed | 驱动值: 21 |
| `RGB_BGRX888` | BGRX8888 packed | 驱动值: 23 |
| `RGB_RGB888_PLANAR` | RGB888 planar | 驱动值: 2 |
| `RGB_BGR888_PLANAR` | BGR888 planar | 驱动值: 4 |
| `RGB_R16G16B16` | RGB 16-bit per channel | 驱动值: 17 |
| `RGB_B16G16R16` | BGR 16-bit per channel | 驱动值: 19 |
| `RGB_GBRP` | GBR planar | 驱动值: 28 |

**使用示例**：
```cpp
// 通道0设置 YUV NV12
TacoConfigBuilder()
    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12)
    .build();

// 通道1设置 RGB BGRA888
TacoConfigBuilder()
    .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGRA888)
    .build();
```

---

### ColorStandard

颜色标准枚举（定义视频颜色空间标准和范围）。

| 枚举值 | 含义 | 说明 |
|--------|------|------|
| `NONE` | 无颜色标准 | - |
| `BT601` | BT.601 full range | 标清 (SD) |
| `BT601_LIMITED` | BT.601 limited range | 标清 (SD) 受限范围 |
| `BT709` | BT.709 full range | 高清 (HD) |
| `BT709_LIMITED` | BT.709 limited range | 高清 (HD) 受限范围 |
| `BT2020` | BT.2020 full range | 超高清 (UHD/HDR) |
| `BT2020_LIMITED` | BT.2020 limited range | 超高清 (UHD/HDR) 受限范围 |

**使用示例**：
```cpp
TacoConfigBuilder()
    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT709)
    .build();
```

---

## 配置结构体

### WorkerConfig

顶层配置结构体，包含 Worker 需要的所有配置。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `data_source` | `DataSourceConfig` | 数据源配置 |
| `display` | `DisplayConfig` | 显示设备配置 |
| `decoder` | `DecoderConfig` | 解码器配置 |
| `worker_type` | `WorkerType` | Worker 类型（默认：`AUTO`） |
| `consumer_type` | `ConsumerTypeConfig` | 消费类型配置（v2.24 新增） |
| `thread_pool_size` | `int` | 全局线程池大小（默认：64，范围：1-128） |

**说明**：
- ⭐ v2.24 重构：`consumer` 重命名为 `consumer_type`，`ConsumerConfig` 重命名为 `ConsumerTypeConfig`
- ⭐ v2.22 重构：数据源相关配置统一归属 `DataSourceConfig`
- `buffer_mode`, `shared_packet_source`, `codec_params`, `time_base` 从 `DecoderConfig` 移至 `DataSourceConfig`

---

### ConsumerTypeConfig（v2.24 新增）

消费类型配置结构体（`WorkerConfig::ConsumerTypeConfig`）。

#### 执行控制

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `max_frames` | `int` | 最大处理帧数（-1=无限制） |
| `max_duration_seconds` | `double` | 最大执行时长（秒，-1=无限制） |
| `timeout_ms` | `int` | 单次获取 Buffer 超时（毫秒，默认：100） |
| `max_timeout_count` | `int` | 最大连续超时次数（默认：10） |
| `verbose` | `bool` | 是否输出详细日志 |

#### 消费类型

##### DisplayType（显示消费类型）

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `enable` | `bool` | 是否启用显示（默认：false） |
| `device_id` | `int` | Framebuffer 设备 ID（默认：0） |

##### SaveRawType（保存原始数据消费类型）

用于保存解码后的 YUV/RGB 数据，调用 `BufferWriter::openRaw()`。

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `enable` | `bool` | 是否启用保存原始数据（默认：false） |
| `output_path` | `std::string` | 输出文件路径（如 output.yuv） |
| `max_frames` | `int` | 最大保存帧数（-1=全部） |

##### SaveEncodedType（保存编码数据消费类型）

用于保存未解码的 H.264/H.265 packet，调用 `BufferWriter::openEncoded()`。

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `enable` | `bool` | 是否启用保存编码数据（默认：false） |
| `output_path` | `std::string` | 输出文件路径（如 output.mp4） |

##### CompareType（比较消费类型）

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `enable` | `bool` | 是否启用比较（默认：false） |
| `enable_psnr` | `bool` | 是否计算 PSNR（默认：true） |
| `min_psnr` | `double` | PSNR 阈值（dB，默认：30.0） |
| `enable_ssim` | `bool` | 是否计算 SSIM（默认：true） |
| `min_ssim` | `double` | SSIM 阈值（0.0-1.0，默认：0.95） |
| `reference_path` | `std::string` | 参考文件路径（可选） |

##### PerformanceType（性能验证消费类型）

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `enable` | `bool` | 是否启用性能验证（默认：false） |
| `target_fps` | `double` | 目标帧率（默认：30.0） |

##### CountType（统计消费类型）

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `enable` | `bool` | 是否启用统计（默认：false） |

#### 使用示例

```cpp
// 直接配置
WorkerConfig config;
config.consumer_type.max_frames = 100;
config.consumer_type.display.enable = true;
config.consumer_type.display.device_id = 0;
config.consumer_type.save_raw.enable = true;
config.consumer_type.save_raw.output_path = "/tmp/output.yuv";
config.consumer_type.save_raw.max_frames = 50;

// 使用 Builder
auto config = WorkerConfigBuilder()
    .setMaxFrames(100)
    .enableDisplay(true, 0)
    .enableSaveRaw(true, "/tmp/output.yuv", 50)
    .build();
```

---

### DataSourceConfig

数据源配置结构体（`WorkerConfig::DataSourceConfig`）。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `path` | `std::string` | 数据源路径/URL（RTSP/HTTP/文件等） |
| `buffer_count` | `int` | BufferPool 的 Buffer 数量（0=使用 Worker 默认值） |
| `buffer_mode` | `bool` | 数据源模式（true=从Buffer获取packet, false=从文件读取） |
| `codec_params` | `const AVCodecParameters*` | Buffer模式下的编解码器参数（从Record Worker获取） |
| `time_base` | `AVRational` | 时间基准（从Record Worker获取，用于同步） |
| `shared_packet_source` | `std::shared_ptr<IPacketSource>` | 共享的 Packet 数据源（共享模式使用） |

**建议值**（`buffer_count`）：
- RTSP 流解码：4-8
- 本地文件解码：128
- Packet 录制：64

**使用场景**（`shared_packet_source`）：
- 普通模式：`nullptr`（Worker 自己创建独立的 BufferPacketSource）
- 共享模式：`MultiWorkerProductionLine` 创建唯一实例并传入

---

### DisplayConfig

显示设备配置结构体（`WorkerConfig::DisplayConfig`）。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `width` | `int` | 显示设备宽度（像素） |
| `height` | `int` | 显示设备高度（像素） |
| `bits_per_pixel` | `int` | 每像素位数（用于BufferPool内存计算） |

**⚠️ 注意**：此配置指定的是**显示设备分辨率**，不是解码器输出分辨率！解码器输出分辨率请使用 `TacoConfig::setScale()`。

---

### DecoderConfig

解码器配置结构体（`WorkerConfig::DecoderConfig`）。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `name` | `std::optional<std::string>` | 解码器名称（`std::nullopt`=自动选择） |
| `enable_hardware` | `bool` | 启用硬件加速（默认：`true`） |
| `hwaccel_device` | `std::optional<std::string>` | 硬件设备（如 "cuda:0", "vaapi"） |
| `decode_threads` | `int` | 解码线程数（0=自动） |
| `taco` | `TacoConfig` | TACO 解码器特定配置 |

---

### TacoConfig

TACO 解码器特定配置结构体（`WorkerConfig::DecoderConfig::TacoConfig`）。

#### 解码器行为配置

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `reorder_disable` | `bool` | 禁用重排序（推荐保持 `true`） |

#### 通道0配置（Channel 0 - YUV Output）

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `ch0_enable` | `bool` | 启用通道0（默认：`true`） |
| `ch0_yuv_format` | `int` | YUV格式类型（-1=自动，0=NV12, 1=NV21, 等） |
| `ch0_yuv_std` | `int` | YUV颜色标准（默认 1=BT.601） |
| `ch0_crop_x` | `int` | 裁剪起始X坐标（0=不裁剪） |
| `ch0_crop_y` | `int` | 裁剪起始Y坐标（0=不裁剪） |
| `ch0_crop_width` | `int` | 裁剪宽度（0=不裁剪） |
| `ch0_crop_height` | `int` | 裁剪高度（0=不裁剪） |
| `ch0_scale_width` | `int` | 缩放目标宽度（0=不缩放） |
| `ch0_scale_height` | `int` | 缩放目标高度（0=不缩放） |

#### 通道1配置（Channel 1 - RGB/YUV Output）

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `ch1_enable` | `bool` | 启用通道1（默认：`false`） |
| `ch1_rgb` | `bool` | 是否输出RGB格式（false=YUV） |
| `ch1_rgb_format` | `int` | RGB格式类型（默认 9=argb888 packed） |
| `ch1_rgb_std` | `int` | RGB颜色标准（默认 1=BT.601 full range） |
| `ch1_yuv_format` | `int` | YUV格式类型（-1=自动） |
| `ch1_yuv_std` | `int` | YUV颜色标准（默认 1=BT.601） |
| `ch1_crop_x` | `int` | 裁剪起始X坐标（0=不裁剪） |
| `ch1_crop_y` | `int` | 裁剪起始Y坐标（0=不裁剪） |
| `ch1_crop_width` | `int` | 裁剪宽度（0=不裁剪） |
| `ch1_crop_height` | `int` | 裁剪高度（0=不裁剪） |
| `ch1_scale_width` | `int` | 缩放目标宽度（0=不缩放） |
| `ch1_scale_height` | `int` | 缩放目标高度（0=不缩放） |

---

## Builder 类

### DataSourceConfigBuilder

数据源配置构建器。

#### 成员函数

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `setPath(std::string_view path)` | `DataSourceConfigBuilder&` | 设置数据源路径/URL |
| `setPath(const char* path)` | `DataSourceConfigBuilder&` | 设置数据源路径（兼容 C 字符串） |
| `setPath(const std::string& path)` | `DataSourceConfigBuilder&` | 设置数据源路径（兼容 std::string） |
| `setBufferCount(int count)` | `DataSourceConfigBuilder&` | 设置 BufferPool 的 Buffer 数量 |
| `build()` | `WorkerConfig::DataSourceConfig` | 构建最终配置 |

#### 使用示例

```cpp
auto dataSource = DataSourceConfigBuilder()
    .setPath("rtsp://192.168.1.100/stream")
    .setBufferCount(8)
    .build();
```

**支持的数据源类型**：
- RTSP 流：`rtsp://192.168.1.100/stream`
- HTTP/HLS 流：`http://example.com/playlist.m3u8`
- 本地文件：`/data/video.mp4`

---

### DisplayConfigBuilder

显示设备配置构建器。

#### 成员函数

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `setDisplayWidth(int width)` | `DisplayConfigBuilder&` | 设置显示设备宽度 |
| `setDisplayHeight(int height)` | `DisplayConfigBuilder&` | 设置显示设备高度 |
| `setDisplayResolution(int width, int height)` | `DisplayConfigBuilder&` | 设置显示设备分辨率 |
| `setBitsPerPixel(int bpp)` | `DisplayConfigBuilder&` | 设置每像素位数 |
| `build()` | `WorkerConfig::DisplayConfig` | 构建最终配置 |

#### 使用示例

```cpp
auto display = DisplayConfigBuilder()
    .setDisplayResolution(1920, 1080)  // Framebuffer 分辨率
    .setBitsPerPixel(32)               // ARGB8888
    .build();
```

---

### TacoConfigBuilder

TACO 解码器特定配置构建器。

#### 成员函数

##### 解码器行为配置

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `setReorderDisable(bool disable = true)` | `TacoConfigBuilder&` | 设置是否禁用重排序 |
| `setChannels(bool ch0, bool ch1)` | `TacoConfigBuilder&` | 同时设置两个通道的启用状态 |

##### 通用配置接口（支持任意通道）

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `setOutputFormat(Channel ch, OutputFormat format, ColorStandard std)` | `TacoConfigBuilder&` | 设置通道输出格式 |
| `setCrop(Channel ch, int x, int y, int width, int height)` | `TacoConfigBuilder&` | 设置通道裁剪区域 |
| `setScale(Channel ch, int width, int height)` | `TacoConfigBuilder&` | 设置通道缩放分辨率 |
| `build()` | `WorkerConfig::DecoderConfig::TacoConfig` | 构建最终配置 |

##### 静态辅助函数（向后兼容）

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `mapFormatNameToEnum(std::string_view name)` | `OutputFormat` | 将格式名称字符串映射为枚举 |
| `mapColorStdNameToEnum(std::string_view name)` | `ColorStandard` | 将颜色标准名称字符串映射为枚举 |
| `mapFormatEnumToName(OutputFormat format)` | `std::string_view` | 将枚举映射为格式名称字符串 |
| `mapColorStdEnumToName(ColorStandard std)` | `std::string_view` | 将枚举映射为颜色标准名称字符串 |

#### 使用示例

##### 示例 1：通道0 YUV NV12 输出

```cpp
auto taco = TacoConfigBuilder()
    .setChannels(true, false)  // 启用通道0，禁用通道1
    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT709)
    .setScale(Channel::CH0, 1920, 1080)
    .build();
```

##### 示例 2：通道1 RGB BGRA888 输出

```cpp
auto taco = TacoConfigBuilder()
    .setChannels(false, true)  // 禁用通道0，启用通道1
    .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGRA888, ColorStandard::BT709)
    .setCrop(Channel::CH1, 0, 0, 1920, 1080)
    .setScale(Channel::CH1, 1280, 720)
    .build();
```

##### 示例 3：双通道输出

```cpp
auto taco = TacoConfigBuilder()
    .setChannels(true, true)  // 同时启用两个通道
    // 通道0: YUV NV12
    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT709)
    .setScale(Channel::CH0, 1920, 1080)
    // 通道1: RGB ARGB888
    .setOutputFormat(Channel::CH1, OutputFormat::RGB_ARGB888, ColorStandard::BT709)
    .setScale(Channel::CH1, 1280, 720)
    .build();
```

**⚠️ 注意**：
- 通道0仅支持 YUV 格式，传入 RGB 格式会记录错误并忽略
- 通道1支持 RGB 和 YUV 格式
- 自动根据 `format` 判断是 RGB 还是 YUV 并设置对应参数

---

### DecoderConfigBuilder

解码器配置构建器。

#### 成员函数

##### 通用解码器参数

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `setDecoderName(std::string_view name)` | `DecoderConfigBuilder&` | 设置解码器名称 |
| `setDecoderName(const char* name)` | `DecoderConfigBuilder&` | 设置解码器名称（兼容 C 字符串） |
| `clearDecoderName()` | `DecoderConfigBuilder&` | 清除解码器名称（使用自动选择） |
| `setHwaccelDevice(std::string_view device)` | `DecoderConfigBuilder&` | 设置硬件加速设备 |
| `setHwaccelDevice(const char* device)` | `DecoderConfigBuilder&` | 设置硬件加速设备（兼容 C 字符串） |
| `setDecodeThreads(int threads)` | `DecoderConfigBuilder&` | 设置解码线程数 |
| `build()` | `WorkerConfig::DecoderConfig` | 构建最终配置 |

##### 快捷预设

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `useTaco(std::string_view codec, const TacoConfig& config)` | `DecoderConfigBuilder&` | 预设：TACO 硬件解码 |
| `useSoftware()` | `DecoderConfigBuilder&` | 预设：软件解码 |
| `useCuvid(std::string_view codec)` | `DecoderConfigBuilder&` | 预设：NVIDIA CUDA 硬件解码 |
| `useQsv(std::string_view codec)` | `DecoderConfigBuilder&` | 预设：Intel Quick Sync Video 硬件解码 |
| `useVaapi(std::string_view codec)` | `DecoderConfigBuilder&` | 预设：VA-API 硬件解码 |

#### 使用示例

##### 示例 1：TACO 硬件解码

```cpp
auto tacoConfig = TacoConfigBuilder()
    .setChannels(true, false)
    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12)
    .build();

auto decoder = DecoderConfigBuilder()
    .useTaco("h264", tacoConfig)
    .build();
```

##### 示例 2：软件解码

```cpp
auto decoder = DecoderConfigBuilder()
    .useSoftware()
    .build();
```

##### 示例 3：NVIDIA CUDA 解码

```cpp
// H.264 CUDA 解码
auto decoder = DecoderConfigBuilder()
    .useCuvid("h264")
    .build();

// H.265/HEVC CUDA 解码
auto decoder = DecoderConfigBuilder()
    .useCuvid("h265")
    .build();
```

##### 示例 4：Intel QSV 解码

```cpp
// H.264 QSV 解码
auto decoder = DecoderConfigBuilder()
    .useQsv("h264")
    .build();
```

##### 示例 5：VA-API 解码

```cpp
// H.264 VA-API 解码
auto decoder = DecoderConfigBuilder()
    .useVaapi("h264")
    .build();
```

**支持的编解码器类型**（`codec` 参数）：
- `"h264"`: H.264/AVC
- `"h265"`: H.265/HEVC
- `"vp9"`: VP9
- `"av1"`: AV1

**生成的解码器名称格式**：
- TACO: `{codec}_taco`（如 `h264_taco`）
- CUDA: `{codec}_cuvid`（如 `h264_cuvid`）
- QSV: `{codec}_qsv`（如 `h264_qsv`）
- VA-API: `{codec}_vaapi`（如 `h264_vaapi`）

---

### WorkerConfigBuilder

Worker 配置构建器（顶层）。

#### 基础配置

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `setDataSourceConfig(const DataSourceConfig& config)` | `WorkerConfigBuilder&` | 设置数据源配置 |
| `setDisplayConfig(const DisplayConfig& config)` | `WorkerConfigBuilder&` | 设置显示设备配置 |
| `setDecoderConfig(const DecoderConfig& config)` | `WorkerConfigBuilder&` | 设置解码器配置 |
| `setWorkerType(WorkerType type)` | `WorkerConfigBuilder&` | 设置 Worker 类型 |
| `setThreadPoolSize(int size)` | `WorkerConfigBuilder&` | 设置全局线程池大小 |
| `build()` | `WorkerConfig` | 构建最终配置 |

#### 消费类型配置（v2.24 新增）

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `setConsumerTypeConfig(const ConsumerTypeConfig& config)` | `WorkerConfigBuilder&` | 设置完整消费类型配置 |
| `setMaxFrames(int frames)` | `WorkerConfigBuilder&` | 设置最大处理帧数 |
| `enableDisplay(bool enable, int device_id)` | `WorkerConfigBuilder&` | 启用显示消费类型 |
| `enableSaveRaw(bool enable, const std::string& path, int max_frames)` | `WorkerConfigBuilder&` | 启用保存原始数据消费类型 |
| `enableSaveEncoded(bool enable, const std::string& path)` | `WorkerConfigBuilder&` | 启用保存编码数据消费类型 |
| `enableCompare(bool enable, double min_psnr, double min_ssim)` | `WorkerConfigBuilder&` | 启用比较消费类型 |
| `enablePerformance(bool enable, double target_fps)` | `WorkerConfigBuilder&` | 启用性能验证消费类型 |
| `setVerbose(bool verbose)` | `WorkerConfigBuilder&` | 设置详细日志模式 |

#### 使用示例

```cpp
auto config = WorkerConfigBuilder()
    .setDataSourceConfig(
        DataSourceConfigBuilder()
            .setPath("rtsp://192.168.1.100/stream")
            .setBufferCount(8)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useTaco("h264", 
                TacoConfigBuilder()
                    .setChannels(true, false)
                    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12)
                    .build()
            )
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_DECODE)
    .setThreadPoolSize(32)
    .build();
```

**线程池大小验证规则**：
- 必须 > 0，否则使用默认值 64
- 最大 128，超过则使用 128
- 0 表示使用默认值 64
- ⚠️ 注意：只在第一次调用时生效，如果线程池已初始化则忽略

---

## MultiWorker 配置

### ProducerConfig

生产者配置结构体。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `producer_name` | `std::string` | 生产者名称（组内唯一标识） |
| `worker_config` | `WorkerConfig` | Worker 配置 |

#### 使用示例

```cpp
ProducerConfig producer;
producer.producer_name = "hw_recorder";
producer.worker_config = WorkerConfigBuilder()
    .setDataSourceConfig(...)
    .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
    .build();
```

---

### ConsumerConfig

消费者配置结构体。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `consumer_name` | `std::string` | 消费者名称（组内唯一标识） |
| `worker_config` | `WorkerConfig` | Worker 配置 |

#### 使用示例

```cpp
ConsumerConfig consumer;
consumer.consumer_name = "hw_decoder";
consumer.worker_config = WorkerConfigBuilder()
    .setDecoderConfig(...)
    .setWorkerType(WorkerType::FFMPEG_DECODE)
    .build();
```

---

### Connector

连接器类，定义生产者-消费者之间的映射规则。

#### 连接器模式（Connector::Mode）

| 枚举值 | 含义 | 说明 |
|--------|------|------|
| `ONE_TO_ONE` | 1:1 映射 | 一个生产者对应一个消费者 |
| `ONE_TO_MANY` | 1:N 映射 | 一个生产者对应多个消费者（广播模式） |
| `MANY_TO_ONE` | N:1 映射 | 多个生产者对应一个消费者（合并模式） |
| `MANY_TO_MANY` | N:M 映射 | 多个生产者对应多个消费者（轮询策略） |

#### 成员函数

| 函数签名 | 返回值 | 说明 |
|----------|--------|------|
| `Connector(Mode mode, const std::vector<std::string>& producers, const std::vector<std::string>& consumers)` | - | 构造函数 |
| `getProducerNameForConsumer(const std::string& consumer_name)` | `std::string` | 获取消费者对应的生产者名称 |
| `containsProducer(const std::string& producer_name)` | `bool` | 检查是否包含指定生产者 |
| `containsConsumer(const std::string& consumer_name)` | `bool` | 检查是否包含指定消费者 |
| `getMode()` | `Mode` | 获取连接器模式 |
| `getProducerNames()` | `const std::vector<std::string>&` | 获取生产者名称列表 |
| `getConsumerNames()` | `const std::vector<std::string>&` | 获取消费者名称列表 |
| `setSharedSource(const std::string& producer_name, std::shared_ptr<IPacketSource> source)` | `void` | 为指定生产者设置共享的 PacketSource |
| `getSharedSource(const std::string& producer_name)` | `std::shared_ptr<IPacketSource>` | 获取指定生产者的共享 PacketSource |

#### 使用示例

```cpp
// ONE_TO_MANY: 一个生产者对应多个消费者
Connector connector(
    Connector::Mode::ONE_TO_MANY,
    {"hw_recorder"},                    // 生产者
    {"hw_decoder", "sw_decoder"}        // 消费者
);

// 查询消费者对应的生产者
std::string producer = connector.getProducerNameForConsumer("hw_decoder");
// producer = "hw_recorder"
```

---

### ConnectorConfig

连接器配置结构体。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `mode` | `Connector::Mode` | 连接器模式 |
| `producer_names` | `std::vector<std::string>` | 关联的生产者名称列表 |
| `consumer_names` | `std::vector<std::string>` | 关联的消费者名称列表 |
| `enable_frame_sync` | `bool` | 是否启用帧同步（v2.23 新增，默认：`false`） |
| `callback_chain` | `CallbackChain` | 回调链（v2.23 新增，可选） |

#### 使用示例

```cpp
ConnectorConfig conn_cfg;
conn_cfg.mode = Connector::Mode::ONE_TO_MANY;
conn_cfg.producer_names = {"hw_recorder"};
conn_cfg.consumer_names = {"hw_decoder", "sw_decoder"};

// v2.23 新增：启用帧同步
conn_cfg.enable_frame_sync = true;
conn_cfg.callback_chain = {
    CallbackChainItem{psnrCallback, &ctx, "PSNR对比"}
};
```

**帧同步功能说明**（v2.23）：
- 允许在 Worker 解码完成后、提交 Buffer 前执行用户自定义的回调函数
- 支持回调链（多个回调按顺序执行）
- 使用场景：PSNR 对比、质量检测、数据聚合等
- 详见：`FRAME_SYNC_EXAMPLE.md`

---

### WorkerGroupConfig

Worker 工作组配置结构体。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `group_id` | `std::string` | 组标识 |
| `producer_configs` | `std::vector<ProducerConfig>` | 多个生产者配置 |
| `consumer_configs` | `std::vector<ConsumerConfig>` | 多个消费者配置 |
| `connector_configs` | `std::vector<ConnectorConfig>` | 多个连接器配置 |

#### 使用示例

```cpp
WorkerGroupConfig group;
group.group_id = "decoder_group";

// 添加生产者
ProducerConfig producer;
producer.producer_name = "hw_recorder";
producer.worker_config = WorkerConfigBuilder()...build();
group.producer_configs.push_back(producer);

// 添加消费者
ConsumerConfig consumer1;
consumer1.consumer_name = "hw_decoder";
consumer1.worker_config = WorkerConfigBuilder()...build();
group.consumer_configs.push_back(consumer1);

// 添加连接器
ConnectorConfig connector;
connector.mode = Connector::Mode::ONE_TO_MANY;
connector.producer_names = {"hw_recorder"};
connector.consumer_names = {"hw_decoder", "sw_decoder"};
group.connector_configs.push_back(connector);
```

**核心概念**：
- 一个 Group = 多个生产者 + 多个消费者 + 多个连接器
- Group 内强同步：通过连接器建立生产者-消费者关系
- Group 间独立：多个 Group 并行运行，互不干扰

---

### MultiWorkerConfig

多 Worker 配置结构体。

#### 成员变量

| 变量名 | 类型 | 含义 |
|--------|------|------|
| `groups` | `std::vector<WorkerGroupConfig>` | Worker Group 配置列表 |
| `thread_pool_size` | `int` | 全局线程池大小（默认：64，范围：1-128） |

#### 使用示例

```cpp
MultiWorkerConfig config;
config.thread_pool_size = 64;

// 添加多个 Group
WorkerGroupConfig group1;
group1.group_id = "decoder_group_1";
// ... 配置 group1
config.groups.push_back(group1);

WorkerGroupConfig group2;
group2.group_id = "decoder_group_2";
// ... 配置 group2
config.groups.push_back(group2);

// 创建 MultiWorkerProductionLine
MultiWorkerProductionLine line(config);
```

---

## 完整示例

### 示例 1：单 Worker RTSP 解码

```cpp
#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"

int main() {
    // 配置 Worker
    auto config = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath("rtsp://192.168.1.100/stream")
                .setBufferCount(8)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", 
                    TacoConfigBuilder()
                        .setChannels(true, false)
                        .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT709)
                        .setScale(Channel::CH0, 1920, 1080)
                        .build()
                )
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_DECODE)
        .setThreadPoolSize(32)
        .build();
    
    // 创建生产线
    VideoProductionLine line(config);
    
    if (!line.start()) {
        std::cerr << "Failed to start production line" << std::endl;
        return 1;
    }
    
    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();
    
    line.stop();
    return 0;
}
```

---

### 示例 2：MultiWorker 架构（一个录制器 + 两个解码器）

```cpp
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"

int main() {
    // ========== 配置 WorkerGroup ==========
    WorkerGroupConfig group;
    group.group_id = "decoder_group";
    
    // ========== 配置生产者（Record Worker）==========
    ProducerConfig producer;
    producer.producer_name = "hw_recorder";
    producer.worker_config = WorkerConfigBuilder()
        .setDataSourceConfig(
            DataSourceConfigBuilder()
                .setPath("rtsp://192.168.1.100/stream")
                .setBufferCount(64)
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_PACKET_RECORDER)
        .build();
    group.producer_configs.push_back(producer);
    
    // ========== 配置消费者1（硬件解码器）==========
    ConsumerConfig consumer1;
    consumer1.consumer_name = "hw_decoder";
    consumer1.worker_config = WorkerConfigBuilder()
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useTaco("h264", 
                    TacoConfigBuilder()
                        .setChannels(true, false)
                        .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12)
                        .setScale(Channel::CH0, 1920, 1080)
                        .build()
                )
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_DECODE)
        .build();
    group.consumer_configs.push_back(consumer1);
    
    // ========== 配置消费者2（软件解码器）==========
    ConsumerConfig consumer2;
    consumer2.consumer_name = "sw_decoder";
    consumer2.worker_config = WorkerConfigBuilder()
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useSoftware()
                .build()
        )
        .setWorkerType(WorkerType::FFMPEG_DECODE)
        .build();
    group.consumer_configs.push_back(consumer2);
    
    // ========== 配置连接器（ONE_TO_MANY）==========
    ConnectorConfig connector;
    connector.mode = Connector::Mode::ONE_TO_MANY;
    connector.producer_names = {"hw_recorder"};
    connector.consumer_names = {"hw_decoder", "sw_decoder"};
    group.connector_configs.push_back(connector);
    
    // ========== 配置 MultiWorkerConfig ==========
    MultiWorkerConfig config;
    config.thread_pool_size = 64;
    config.groups.push_back(group);
    
    // ========== 创建并启动 MultiWorkerProductionLine ==========
    MultiWorkerProductionLine line(config);
    
    if (!line.start()) {
        std::cerr << "Failed to start production line" << std::endl;
        return 1;
    }
    
    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();
    
    line.stop();
    return 0;
}
```

---

### 示例 3：双通道输出（YUV + RGB）

```cpp
auto config = WorkerConfigBuilder()
    .setDataSourceConfig(
        DataSourceConfigBuilder()
            .setPath("rtsp://192.168.1.100/stream")
            .setBufferCount(8)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useTaco("h264", 
                TacoConfigBuilder()
                    .setChannels(true, true)  // 同时启用两个通道
                    // 通道0: YUV NV12 输出
                    .setOutputFormat(Channel::CH0, OutputFormat::YUV_NV12, ColorStandard::BT709)
                    .setScale(Channel::CH0, 1920, 1080)
                    // 通道1: RGB BGRA888 输出
                    .setOutputFormat(Channel::CH1, OutputFormat::RGB_BGRA888, ColorStandard::BT709)
                    .setScale(Channel::CH1, 1280, 720)
                    .build()
            )
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_DECODE)
    .build();
```

---

### 示例 4：帧同步 + PSNR 对比（v2.23）

```cpp
#include "productionline/MultiWorkerProductionLine.hpp"
#include "productionline/WorkerSyncCoordinator.hpp"

// ========== 定义 PSNR 回调 ==========
struct PSNRContext {
    double threshold = 30.0;
    std::atomic<int> mismatch_count{0};
};

bool psnrCallback(
    uint64_t frame_version,
    const std::map<std::string, Buffer*>& worker_buffers,
    void* ctx
) {
    auto* context = static_cast<PSNRContext*>(ctx);
    
    // 获取两个 Worker 的 Buffer
    auto it1 = worker_buffers.find("hw_decoder");
    auto it2 = worker_buffers.find("sw_decoder");
    
    if (it1 == worker_buffers.end() || it2 == worker_buffers.end()) {
        return false;
    }
    
    // 计算 PSNR
    double psnr = calculatePSNR(it1->second, it2->second);
    
    if (psnr < context->threshold) {
        context->mismatch_count++;
        return false;  // 拒绝提交
    }
    
    return true;  // 允许提交
}

int main() {
    PSNRContext psnr_ctx;
    
    // ========== 配置连接器（启用帧同步）==========
    ConnectorConfig connector;
    connector.mode = Connector::Mode::ONE_TO_MANY;
    connector.producer_names = {"hw_recorder"};
    connector.consumer_names = {"hw_decoder", "sw_decoder"};
    
    // ⭐ 启用帧同步
    connector.enable_frame_sync = true;
    connector.callback_chain.push_back(CallbackChainItem{
        psnrCallback,
        &psnr_ctx,
        "PSNR对比"
    });
    
    // ... 其他配置 ...
    
    MultiWorkerProductionLine line(config);
    line.start();
    
    std::cin.get();
    line.stop();
    
    std::cout << "不匹配帧数: " << psnr_ctx.mismatch_count << std::endl;
    return 0;
}
```

---

## 版本历史

| 版本 | 日期 | 变更内容 |
|------|------|----------|
| v2.24 | 2026-01-29 | 重构 `ConsumerConfig` 为 `ConsumerTypeConfig`，使用嵌套结构体分类消费类型 |
| v2.23 | 2026-01-23 | 新增帧同步功能（`WorkerSyncCoordinator`、`ConnectorConfig::enable_frame_sync`） |
| v2.22 | 2026-01-22 | 重构数据源配置，修复 `BufferPacketSource` 资源泄漏和死锁 |
| v2.21 | 2026-01-21 | 重构 `Connector` 为使用名字而非索引 |
| v2.20 | 2026-01-20 | 配置结构从 `MultiWorkerProductionLine` 移动到 `WorkerConfig.hpp` |
| v2.18 | 2026-01-18 | 引入共享模式（ONE_TO_MANY） |

---

## 常见问题

### Q1: 如何选择解码器？

**推荐顺序**：
1. **TACO 硬件解码**（如果在 TACO 平台）：性能最佳
2. **CUDA/QSV/VA-API 硬件解码**：根据硬件平台选择
3. **软件解码**：兼容性最好，但性能较低

### Q2: 通道0和通道1有什么区别？

- **通道0**：仅支持 YUV 格式输出
- **通道1**：支持 RGB 和 YUV 格式输出，功能更强大

### Q3: 如何设置解码器输出分辨率？

使用 `TacoConfigBuilder::setScale()`：

```cpp
TacoConfigBuilder()
    .setScale(Channel::CH0, 1920, 1080)  // 缩放到 1920x1080
    .build();
```

**⚠️ 注意**：不要与 `DisplayConfig` 的分辨率混淆，后者是显示设备分辨率。

### Q4: BufferCount 应该设置多少？

**建议值**：
- RTSP 流解码：4-8（低延迟）
- 本地文件解码：128（高吞吐量）
- Packet 录制：64（平衡）

### Q5: 如何启用帧同步功能？

参见 [示例 4](#示例-4帧同步--psnr-对比v223) 和 `FRAME_SYNC_EXAMPLE.md`。

---

## 参考文档

- `FRAME_SYNC_EXAMPLE.md`: 帧同步功能使用示例（v2.23）
- `BUGFIX_v2.22.md`: v2.22 修复报告
- `README.md`: Components 库总体介绍

---

**文档结束**
