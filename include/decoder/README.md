# 解码器系统使用指南

## 概述

本解码器系统采用**工厂模式 + 门面模式**设计，直接使用**FFmpeg原生类型**，提供统一、专业的视频解码接口。

**核心特性**：
- ✅ **零拷贝解码**：FFmpeg通过`get_buffer2`直接使用BufferPool，消除内存拷贝
- ✅ **FFmpeg标准**：直接使用`AVPixelFormat`、`AVFrame`等原生类型
- ✅ **深度集成**：与BufferPool无缝对接，支持DMA显示
- ✅ **专业接口**：send/receive分离模式，对齐FFmpeg API
- ✅ **大厂实践**：参考Chromium、GStreamer的零拷贝实现

## 架构设计

```
┌─────────────────────────────────────────────────┐
│                    Decoder                       │  门面类（Facade）
│  - 统一接口                                      │
│  - 生命周期管理                                  │
│  - 配置管理                                      │
└────────────────┬────────────────────────────────┘
                 │ 持有
                 ▼
┌─────────────────────────────────────────────────┐
│                  IDecoder*                       │  接口指针
└────────────────┬────────────────────────────────┘
                 │ 多态
        ┌────────┴────────┬───────────────┐
        ▼                 ▼               ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│FFmpegDecoder │  │HardwareDecoder│  │VaapiDecoder  │  具体实现
│(软件解码)    │  │(通用硬件加速) │  │(Intel/AMD)   │
└──────────────┘  └──────────────┘  └──────────────┘
```

## 核心组件

### 1. IDecoder - 解码器接口

定义所有解码器必须实现的标准接口：
- `initialize()` - 初始化解码器
- `sendPacket()` - 发送编码数据包（FFmpeg标准）
- `receiveFrame()` - 接收解码帧（FFmpeg标准）
- `flush()` - 刷新缓冲区
- `reset()` - 重置状态
- `close()` - 关闭解码器

### 2. DecoderFactory - 工厂类

根据类型创建具体的解码器实例：

```cpp
enum class DecoderType {
    AUTO,           // 自动选择
    FFMPEG,         // FFmpeg软件解码
    HARDWARE,       // 硬件加速
    VAAPI,          // VA-API (Intel/AMD)
    NVDEC,          // NVIDIA
    VIDEOTOOLBOX,   // Apple
    CUSTOM          // 自定义
};
```

### 3. Decoder - 门面类

提供简洁的对外接口，隐藏内部实现复杂度。

### 4. BufferAllocationMode - Buffer分配模式

```cpp
enum class BufferAllocationMode {
    INTERNAL,    // FFmpeg内部分配（有拷贝）
    ZERO_COPY,   // 零拷贝模式（推荐）
    INJECTION    // 注入模式（动态）
};
```

## 零拷贝模式详解

### 工作原理

```
传统模式（有拷贝）：
网络/文件 → FFmpeg内部Buffer → memcpy → BufferPool → Display
                                  ^^^^^ 拷贝开销！

零拷贝模式（本系统）：
网络/文件 → FFmpeg(get_buffer2回调) → 直接写入BufferPool的Buffer → Display
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ 零拷贝！
```

### 核心机制

1. **get_buffer2回调**：FFmpeg需要buffer时调用我们的回调
2. **从BufferPool获取**：回调从BufferPool获取空闲Buffer
3. **直接解码**：FFmpeg直接解码到这个Buffer
4. **引用计数**：通过AVBuffer管理Buffer生命周期
5. **用户使用**：DecodedFrame.buffer指向BufferPool的Buffer

## 快速开始

### 基本用法（零拷贝）

```cpp
#include "decoder/Decoder.hpp"

// 1. 创建BufferPool（预分配）
size_t frame_size = 1920 * 1080 * 3 / 2;  // NV12格式
std::vector<Buffer*> buffers;
for (int i = 0; i < 10; i++) {
    buffers.push_back(new Buffer(i, frame_size));
}
BufferPool pool(buffers, "Decoder_Pool", "Decoder");

// 2. 创建解码器
Decoder decoder(DecoderFactory::DecoderType::FFMPEG);

// 3. 配置（使用FFmpeg原生类型！）
decoder.setCodec(AV_CODEC_ID_H264);           // FFmpeg的codec ID
decoder.setOutputFormat(1920, 1080, AV_PIX_FMT_NV12);  // FFmpeg的像素格式
decoder.setThreadCount(4);

// 4. 关键：启用零拷贝模式
decoder.setBufferMode(BufferAllocationMode::ZERO_COPY);
decoder.attachBufferPool(&pool);

// 5. 初始化
if (decoder.open() != DecoderStatus::OK) {
    printf("Failed: %s\n", decoder.getLastError());
    return;
}

// 6. 解码（send/receive模式）
AVPacket* packet = /* 从文件/网络读取 */;

decoder.sendPacket(packet);

DecodedFrame frame;
while (decoder.receiveFrame(frame) == DecoderStatus::OK) {
    // frame.buffer 直接指向BufferPool的Buffer！
    printf("Decoded frame: pts=%ld, buffer=#%u\n", 
           frame.pts(), frame.buffer->id());
    
    // 零拷贝显示
    display.displayBufferByDMA(frame.buffer);
    
    // 释放AVFrame（重要！）
    frame.release();
    
    // 归还buffer
    pool.releaseFilled(frame.buffer);
}

// 7. 清理
decoder.close();
```

### 与Display集成（完整播放流程）

```cpp
// 1. 创建组件
LinuxFramebufferDevice display;
BufferPool decoder_pool("Decoder_Pool", "Decoder", 10);
Decoder decoder(DecoderFactory::DecoderType::FFMPEG);

// 2. 初始化display
display.initialize(1920, 1080, 32);

// 3. 配置decoder
decoder.setCodec(AV_CODEC_ID_H264);
decoder.setOutputFormat(1920, 1080, AV_PIX_FMT_NV12);
decoder.setBufferMode(BufferAllocationMode::ZERO_COPY);
decoder.attachBufferPool(&decoder_pool);
decoder.open();

// 4. 读取H.264文件并解码
FILE* fp = fopen("video.h264", "rb");
AVPacket* packet = av_packet_alloc();

while (read_h264_packet(fp, packet)) {
    // 发送packet
    decoder.sendPacket(packet);
    
    // 接收所有可用的帧
    DecodedFrame frame;
    while (decoder.receiveFrame(frame) == DecoderStatus::OK) {
        // 零拷贝显示
        display.displayBufferByDMA(frame.buffer);
        
        // 清理
        frame.release();
        decoder_pool.releaseFilled(frame.buffer);
    }
}

// 5. 刷新解码器（获取缓冲的B帧）
DecodedFrame frame;
while (decoder.flush(frame) == DecoderStatus::OK) {
    display.displayBufferByDMA(frame.buffer);
    frame.release();
    decoder_pool.releaseFilled(frame.buffer);
}

// 6. 清理
av_packet_free(&packet);
fclose(fp);
decoder.close();
display.close();
```

## 支持的像素格式

直接使用FFmpeg的`AVPixelFormat`枚举，支持100+种格式，常用的包括：

```cpp
AV_PIX_FMT_NV12      // YUV 4:2:0, 12bpp (推荐，硬件友好)
AV_PIX_FMT_NV21      // YUV 4:2:0, 12bpp
AV_PIX_FMT_YUV420P   // YUV 4:2:0 planar, 12bpp
AV_PIX_FMT_YUYV422   // YUV 4:2:2 packed, 16bpp
AV_PIX_FMT_RGB24     // RGB 8:8:8, 24bpp
AV_PIX_FMT_RGBA      // RGBA 8:8:8:8, 32bpp
```

## 支持的编解码器

使用FFmpeg的`AVCodecID`，支持所有FFmpeg支持的编解码器：

```cpp
AV_CODEC_ID_H264     // H.264/AVC
AV_CODEC_ID_HEVC     // H.265/HEVC
AV_CODEC_ID_VP8      // VP8
AV_CODEC_ID_VP9      // VP9
AV_CODEC_ID_AV1      // AV1
AV_CODEC_ID_MPEG4    // MPEG-4
```

## DecoderStatus 状态码

```cpp
enum class DecoderStatus {
    OK,                    // 成功
    NEED_MORE_DATA,        // 需要更多输入（EAGAIN）
    END_OF_STREAM,         // 流结束（EOF）
    DECODE_ERROR,          // 解码错误
    OUT_OF_MEMORY,         // 内存不足
    NOT_INITIALIZED,       // 未初始化
    UNSUPPORTED_CONFIG,    // 不支持的配置
    // ...
};
```

## 高级用法

### send/receive 分离模式（推荐）

FFmpeg标准模式，处理B帧和多输出：

```cpp
// 发送多个packet
for (int i = 0; i < packet_count; i++) {
    decoder.sendPacket(packets[i]);
}

// 接收所有可用帧（可能多于packet数量）
DecodedFrame frame;
while (decoder.receiveFrame(frame) == DecoderStatus::OK) {
    // 处理帧
    frame.release();
}
```

### 使用FFmpeg原生AVFrame

`DecodedFrame`直接包含`AVFrame*`，可以访问所有FFmpeg元数据：

```cpp
DecodedFrame frame;
if (decoder.receiveFrame(frame) == DecoderStatus::OK) {
    // 访问FFmpeg原生数据
    printf("PTS: %ld\n", frame.av_frame->pts);
    printf("Format: %s\n", av_get_pix_fmt_name((AVPixelFormat)frame.av_frame->format));
    printf("Key frame: %d\n", frame.av_frame->key_frame);
    printf("Width: %d, Height: %d\n", frame.av_frame->width, frame.av_frame->height);
    
    // 也可以用便捷访问器
    printf("PTS: %ld\n", frame.pts());
    printf("Width: %d\n", frame.width());
}
```

### 设置extradata（SPS/PPS等）

```cpp
// H.264需要SPS/PPS
uint8_t extradata[] = { /* SPS/PPS数据 */ };
decoder.setExtraData(extradata, sizeof(extradata));
```

### 设置时间基准

```cpp
AVRational time_base = {1, 90000};  // 90kHz时间基准
decoder.setTimeBase(time_base);
```

## 性能优化建议

1. **使用零拷贝模式**：
   ```cpp
   decoder.setBufferMode(BufferAllocationMode::ZERO_COPY);
   decoder.attachBufferPool(&pool);
   ```

2. **多线程解码**：
   ```cpp
   decoder.setThreadCount(0);  // 0 = 自动，根据CPU核心数
   ```

3. **BufferPool预分配**：
   ```cpp
   // 预分配足够大的buffer
   size_t frame_size = width * height * 3 / 2;  // NV12
   BufferPool pool(buffers, "Decoder", "Pool");
   ```

4. **选择合适的像素格式**：
   - NV12：硬件友好，零拷贝DMA显示
   - YUV420P：通用性好，软件处理友好

## 错误处理

```cpp
DecoderStatus status = decoder.open();
if (status != DecoderStatus::OK) {
    printf("Error: %s\n", decoder.getLastError());
    printf("FFmpeg error code: %d\n", decoder.getLastFFmpegError());
}

status = decoder.receiveFrame(frame);
if (status == DecoderStatus::NEED_MORE_DATA) {
    // 正常，需要发送更多packet
} else if (status == DecoderStatus::END_OF_STREAM) {
    // 流结束
} else if (status != DecoderStatus::OK) {
    printf("Decode error: %s\n", decoder.getLastError());
}
```

## 注意事项

1. **BufferPool大小**：零拷贝模式下，BufferPool必须预分配且buffer大小足够
2. **frame.release()**：使用完DecodedFrame后必须调用`release()`释放AVFrame
3. **buffer归还**：使用完buffer后必须归还给BufferPool
4. **线程安全**：每个Decoder实例独立，不同实例间线程安全
5. **生命周期**：确保BufferPool生命周期长于Decoder

## 对比：传统方式 vs 零拷贝

| 方面 | 传统方式 | 零拷贝模式 |
|------|---------|-----------|
| 内存拷贝 | ✗ 至少1次memcpy | ✅ 0次拷贝 |
| CPU开销 | ✗ 高（拷贝数据） | ✅ 低 |
| 延迟 | ✗ 高 | ✅ 低 |
| 内存占用 | ✗ 2倍（FFmpeg + Pool） | ✅ 1倍（共享） |
| 代码复杂度 | ✅ 简单 | ⚠️ 中等 |
| 适用场景 | 原型开发 | 生产环境 |

## 示例代码

完整示例请参考：
- `test.cpp` 中的 `test_decoder_basic()` 函数
- 演示了零拷贝模式的完整工作流程

## 扩展新解码器

要添加新的解码器实现：

1. **继承IDecoder接口**：
   ```cpp
   class MyDecoder : public IDecoder {
       DecoderStatus initialize(const DecoderConfig& config) override;
       // 实现所有虚函数
   };
   ```

2. **在DecoderFactory中注册**：
   ```cpp
   case DecoderType::CUSTOM:
       return std::make_unique<MyDecoder>();
   ```

3. **使用新解码器**：
   ```cpp
   Decoder decoder(DecoderFactory::DecoderType::CUSTOM);
   ```

## 参考资料

- [FFmpeg API文档](https://ffmpeg.org/doxygen/trunk/index.html)
- [Chromium VideoDecoder](https://source.chromium.org/chromium/chromium/src/+/main:media/base/video_decoder.h)
- [GStreamer BufferPool](https://gstreamer.freedesktop.org/documentation/gstreamer/gstbufferpool.html)

---

**系统完全对齐FFmpeg和大厂实践，提供专业、高性能的解码方案！** 🚀
