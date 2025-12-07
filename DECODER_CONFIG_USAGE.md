# 解码器配置使用指南

## 📋 问题背景

之前的设计中，`FfmpegDecodeVideoFileWorker` 的解码器名称被硬编码为 `"h264_taco"`，导致用户无法灵活配置。

## ✅ 解决方案

采用**配置驱动设计**，通过 `VideoProductionLine::Config` 结构体传递解码器配置，支持三种使用场景：

---

## 🎯 使用场景一：生产线（ProductionLine）配置

### 方式1：使用默认解码器（FFmpeg 自动选择）

```cpp
#include "productionline/VideoProductionLine.hpp"

// 创建配置（不指定 decoder_name，使用默认值 nullptr）
VideoProductionLine::Config config(
    "/path/to/video.mp4",
    1920, 1080, 32,
    true,  // loop
    1,     // thread_count
    BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE
);

// decoder_name 默认为 nullptr，FFmpeg 会自动选择最合适的解码器

VideoProductionLine producer;
producer.start(config);
```

### 方式2：显式指定解码器（推荐用于特殊硬件）

```cpp
// 创建配置并指定解码器
VideoProductionLine::Config config(
    "/path/to/video.mp4",
    1920, 1080, 32,
    true,
    1,
    BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE
);

// 🎯 显式指定解码器
config.decoder_name = "h264_taco";  // 使用 h264_taco 硬件解码器

VideoProductionLine producer;
producer.start(config);
```

### 方式3：根据运行环境动态配置

```cpp
VideoProductionLine::Config config(
    video_path,
    1920, 1080, 32,
    true, 1,
    BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE
);

// 根据环境变量或硬件检测动态配置
const char* env_decoder = getenv("PREFERRED_DECODER");
if (env_decoder && strcmp(env_decoder, "taco") == 0) {
    config.decoder_name = "h264_taco";  // 使用硬件解码器
    printf("Using h264_taco hardware decoder\n");
} else {
    config.decoder_name = nullptr;  // 使用默认解码器
    printf("Using FFmpeg default decoder\n");
}

VideoProductionLine producer;
producer.start(config);
```

---

## 🧪 使用场景二：测试代码直接创建 Worker

### 方式1：通过 Facade 设置

```cpp
#include "productionline/worker/facade/BufferFillingWorkerFacade.hpp"

// 创建 Worker Facade
BufferFillingWorkerFacade worker(
    BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE
);

// 设置解码器（在 open 之前）
worker.setDecoderName("h264_taco");

// 打开视频
worker.open("/path/to/video.mp4");

// 使用 worker...
```

### 方式2：直接创建 Worker

```cpp
#include "productionline/worker/implementation/FfmpegDecodeVideoFileWorker.hpp"

// 直接创建 Worker
FfmpegDecodeVideoFileWorker worker;

// 配置解码器（在 open 之前）
worker.setDecoderName("h264_taco");

// 打开视频
worker.open("/path/to/video.mp4");

// 使用 worker...
```

---

## 🔄 解码器配置对比表

| 解码器名称 | 使用场景 | 性能 | 兼容性 | 配置方式 |
|-----------|---------|------|--------|---------|
| `nullptr` (默认) | 通用场景 | 🟡 中等 | 🟢 最佳 | `config.decoder_name = nullptr;` |
| `"h264_taco"` | TACO 硬件平台 | 🟢 最高 | 🟡 仅TACO | `config.decoder_name = "h264_taco";` |
| `"h264_cuvid"` | NVIDIA GPU | 🟢 高 | 🟡 仅NVIDIA | `config.decoder_name = "h264_cuvid";` |
| `"h264_qsv"` | Intel Quick Sync | 🟢 高 | 🟡 仅Intel | `config.decoder_name = "h264_qsv";` |
| `"h264"` | 纯软件解码 | 🔴 低 | 🟢 最佳 | `config.decoder_name = "h264";` |

---

## 📝 完整示例：生产环境配置

### 示例1：配置文件驱动

```cpp
// config.json
{
    "video_path": "/data/video.mp4",
    "decoder": "h264_taco",
    "width": 1920,
    "height": 1080,
    "loop": true
}

// main.cpp
#include <nlohmann/json.hpp>
#include <fstream>

// 读取配置
std::ifstream config_file("config.json");
nlohmann::json j;
config_file >> j;

// 创建配置
VideoProductionLine::Config config(
    j["video_path"],
    j["width"], j["height"], 32,
    j["loop"],
    1,
    BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE
);

// 设置解码器
config.decoder_name = j["decoder"].get<std::string>().c_str();

// 启动
VideoProductionLine producer;
producer.start(config);
```

### 示例2：命令行参数配置

```cpp
#include <cstring>

int main(int argc, char* argv[]) {
    const char* video_path = argv[1];
    const char* decoder_name = nullptr;
    
    // 解析命令行参数
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--decoder") == 0 && i + 1 < argc) {
            decoder_name = argv[i + 1];
            break;
        }
    }
    
    // 创建配置
    VideoProductionLine::Config config(
        video_path,
        1920, 1080, 32,
        true, 1,
        BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE
    );
    config.decoder_name = decoder_name;
    
    // 启动
    VideoProductionLine producer;
    producer.start(config);
    
    // 运行...
    
    return 0;
}

// 使用示例：
// ./app video.mp4 --decoder h264_taco
// ./app video.mp4  (使用默认解码器)
```

---

## 🏗️ 架构优势

### 1. 配置驱动
- 所有配置集中在 `Config` 结构体
- 易于扩展新的配置项
- 符合"配置与代码分离"原则

### 2. 向后兼容
- 默认值 `nullptr` 表示自动选择
- 不指定 `decoder_name` 时行为与之前一致
- 旧代码无需修改即可工作

### 3. 灵活性
- 支持生产线和测试代码两种场景
- 支持运行时配置
- 支持环境变量和配置文件

### 4. 类型安全
- 编译期检查配置项类型
- 避免运行时配置错误

---

## ⚙️ 技术实现细节

### 设计方案：虚函数 + 默认实现（面向对象经典模式）

#### 关键优势：
1. **符合开放封闭原则**：基类定义接口，子类可选重写
2. **无需 RTTI**：不使用 `dynamic_cast`，性能更好
3. **易于扩展**：新增 Worker 类型无需修改现有代码
4. **类型安全**：编译期检查，避免运行时错误

#### 实现细节：

**1. WorkerBase（基类）**
```cpp
class WorkerBase : public IVideoFileNavigator {
public:
    // 🎯 虚函数 + 默认空实现
    virtual void setDecoderName(const char* decoder_name) {
        // 默认空实现：不支持解码器配置的 Worker 忽略此调用
        (void)decoder_name;
    }
};
```

**2. FfmpegDecodeVideoFileWorker（子类）**
```cpp
class FfmpegDecodeVideoFileWorker : public WorkerBase {
public:
    // 🎯 重写方法，提供实际实现
    void setDecoderName(const char* decoder_name) override {
        if (!is_open_.load(std::memory_order_acquire)) {
            decoder_name_ptr_ = decoder_name;
        }
    }
};
```

**3. MmapRawVideoFileWorker（其他子类）**
```cpp
class MmapRawVideoFileWorker : public WorkerBase {
    // 🎯 不需要重写，自动继承基类的空实现
    // 调用 setDecoderName() 时什么也不做（正确行为）
};
```

**4. BufferFillingWorkerFacade（门面类）**
```cpp
void BufferFillingWorkerFacade::setDecoderName(const char* decoder_name) {
    // 🎯 直接调用基类方法（多态机制）
    // - FfmpegDecodeVideoFileWorker: 执行实际设置
    // - MmapRawVideoFileWorker: 执行空操作（忽略）
    worker_base_uptr_->setDecoderName(decoder_name);
}
```

### 配置传递流程

```
用户代码
   ↓ Config.decoder_name = "h264_taco"
VideoProductionLine.start(config)
   ↓ worker_facade->setDecoderName(config.decoder_name)
BufferFillingWorkerFacade.setDecoderName()
   ↓ worker_base_uptr_->setDecoderName()  // 多态调用
   ├─ FfmpegDecodeVideoFileWorker::setDecoderName()  ✅ 实际设置
   └─ MmapRawVideoFileWorker::setDecoderName()       ✅ 空操作（忽略）
```

### 为什么这个设计优于 dynamic_cast？

| 方面 | dynamic_cast 方案 | 虚函数方案（当前） |
|------|------------------|------------------|
| **性能** | 🟡 需要 RTTI 查询 | 🟢 虚函数表查找（更快） |
| **依赖** | 🟡 需要包含具体类头文件 | 🟢 只需基类头文件 |
| **扩展性** | 🟡 新增类型需修改门面 | 🟢 新增类型自动支持 |
| **OOP 原则** | 🟡 破坏封装性 | 🟢 完美符合多态 |
| **代码复杂度** | 🟡 需要类型判断逻辑 | 🟢 简单直接 |

### 关键修改点

1. **WorkerBase.hpp**（基类添加虚函数）
   - 添加 `virtual void setDecoderName(const char*)`
   - 提供默认空实现

2. **FfmpegDecodeVideoFileWorker.hpp**（子类重写）
   - 添加 `override` 标记
   - 提供实际实现

3. **BufferFillingWorkerFacade.cpp**（门面简化）
   - 去掉 `dynamic_cast`
   - 直接调用基类方法（多态）

4. **VideoProductionLine.cpp**（传递配置）
   - 调用 `worker_facade->setDecoderName(config.decoder_name)`

---

## 🎓 大厂设计经验总结

### 1. 配置结构体模式（Google/Chromium 风格）
- 将所有配置封装在一个结构体中
- 提供合理的默认值
- 支持链式配置

### 2. 门面模式 + 策略模式
- 用户只需与 Facade 交互
- 底层实现可透明切换
- 易于扩展新的 Worker 类型

### 3. 依赖注入
- 配置从外部注入，而非硬编码
- 便于测试和维护
- 支持运行时配置

### 4. 防御性编程
- 使用 `dynamic_cast` 安全转换
- 提供友好的警告信息
- 向后兼容旧代码

---

## 🔍 常见问题

### Q1: 如何查看支持的解码器列表？
```bash
ffmpeg -decoders | grep h264
```

### Q2: 如何知道当前使用的是哪个解码器？
```cpp
producer.start(config);
// 日志会打印：Decoder: h264_taco (user specified)
// 或：Decoder: auto (FFmpeg will choose)
```

### Q3: 如果指定的解码器不存在会怎样？
FFmpeg 会回退到默认解码器，并打印警告信息。

### Q4: 可以在运行时切换解码器吗？
不可以。必须在 `open()` 之前设置解码器。如需切换，请先 `close()`，再重新配置并 `open()`。

---

## 📚 相关文档

- [Worker 子系统架构](include/productionline/worker/Worker-subsystem-architecture.md)
- [VideoProductionLine 使用指南](include/productionline/VideoProductionLine.hpp)
- [FFmpeg 解码器文档](https://ffmpeg.org/ffmpeg-codecs.html)

---

## ✅ 总结

通过将解码器配置添加到 `VideoProductionLine::Config` 结构体，我们实现了：

✅ **灵活配置**：支持自动选择和手动指定  
✅ **向后兼容**：旧代码无需修改  
✅ **架构优雅**：配置驱动 + 依赖注入  
✅ **易于维护**：配置集中管理  
✅ **生产就绪**：支持配置文件、命令行、环境变量

这是一个符合大厂设计标准的解决方案！ 🎉

