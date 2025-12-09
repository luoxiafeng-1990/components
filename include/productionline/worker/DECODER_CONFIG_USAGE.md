# Worker 配置使用指南（v2.3 重构版）

> **文档版本**: v2.3  
> **最后更新**: 2025-12-08  
> **重大变更**: 配置系统完全重构，`WorkerConfig` 独立管理所有 Worker 配置

---

## 📋 问题背景

### v2.2 之前的问题
- 解码器名称被硬编码为 `"h264_taco"`
- 配置分散在多个地方（生产线 Config 和 Worker Config）
- Builder 职责混乱，跨层级设置

### v2.3 重构解决方案
- **配置归位**：所有 Worker 配置统一在 `WorkerConfig` 中
- **职责清晰**：每个 Builder 只负责自己层级的配置
- **API 简化**：生产线参数直接传递，不需要复杂的 Config 结构体

---

## 🎯 WorkerConfig 完整结构

```cpp
struct WorkerConfig {
    // 文件配置
    struct FileConfig {
        const char* file_path = nullptr;       // 文件路径
        int start_frame = 0;                   // 起始帧
        int end_frame = -1;                    // 结束帧（-1=全部）
    } file;
    
    // 输出配置
    struct OutputConfig {
        int width = 0;                         // 输出宽度
        int height = 0;                        // 输出高度
        int bits_per_pixel = 0;                // 每像素位数
    } output;
    
    // 解码器配置
    struct DecoderConfig {
        const char* name = nullptr;            // 解码器名称
        bool enable_hardware = true;           // 启用硬件加速
        const char* hwaccel_device = nullptr;  // 硬件设备
        int decode_threads = 0;                // 解码线程数
        
        // h264_taco 特定配置
        struct TacoConfig {
            bool reorder_disable = true;
            bool ch0_enable = true;
            bool ch1_enable = true;
            bool ch1_rgb = true;
            const char* ch1_rgb_format = "argb888";
            const char* ch1_rgb_std = "bt601";
            int ch1_crop_x = 0;
            int ch1_crop_y = 0;
            int ch1_crop_width = 0;
            int ch1_crop_height = 0;
            int ch1_scale_width = 0;
            int ch1_scale_height = 0;
        } taco;
    } decoder;
    
    // Worker 类型
    WorkerType worker_type = WorkerType::AUTO;
};
```

---

## 🚀 使用场景一：生产线（ProductionLine）配置

### 方式 1：使用预设（最简单）

```cpp
#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"

// 构建 Worker 配置
auto workerConfig = WorkerConfigBuilder()
    .setFileConfig(
        FileConfigBuilder()
            .setFilePath("/path/to/video.mp4")
            .build()
    )
    .setOutputConfig(
        OutputConfigBuilder()
            .setResolution(1920, 1080)
            .setBitsPerPixel(32)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useH264Taco()  // 🎯 使用 h264_taco 预设
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
    .build();

// 启动生产线（简洁明了）
VideoProductionLine pipeline;
pipeline.start(workerConfig, true, 4);  // loop=true, thread_count=4
```

### 方式 2：使用其他解码器预设

```cpp
// NVIDIA CUDA 解码
auto workerConfig = WorkerConfigBuilder()
    .setFileConfig(
        FileConfigBuilder()
            .setFilePath("/path/to/video.h264")
            .build()
    )
    .setOutputConfig(
        OutputConfigBuilder()
            .setResolution(1920, 1080)
            .setBitsPerPixel(32)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useH264Cuvid()  // 🎯 NVIDIA CUDA
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
    .build();

VideoProductionLine pipeline;
pipeline.start(workerConfig, false, 1);

// Intel Quick Sync 解码
auto qsvConfig = WorkerConfigBuilder()
    .setFileConfig(...)
    .setOutputConfig(...)
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useH264Qsv()  // 🎯 Intel QSV
            .build()
    )
    .build();

// 软件解码
auto softConfig = WorkerConfigBuilder()
    .setFileConfig(...)
    .setOutputConfig(...)
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useSoftware()  // 🎯 软件解码
            .build()
    )
    .build();
```

### 方式 3：自定义解码器参数

```cpp
auto workerConfig = WorkerConfigBuilder()
    .setFileConfig(
        FileConfigBuilder()
            .setFilePath("/path/to/video.h264")
            .build()
    )
    .setOutputConfig(
        OutputConfigBuilder()
            .setResolution(1920, 1080)
            .setBitsPerPixel(32)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .setDecoderName("h264_taco")
            .enableHardware(true)
            .setDecodeThreads(4)
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
    .build();

VideoProductionLine pipeline;
pipeline.start(workerConfig, true, 4);
```

### 方式 4：详细配置 h264_taco 参数

```cpp
// 构建 taco 特定配置
auto tacoConfig = TacoConfigBuilder()
    .setReorderDisable(true)
    .setChannels(true, true)
    .setRgbConfig(true, "argb888", "bt601")
    .setCropRegion(0, 0, 1920, 1080)
    .setScaleSize(640, 480)
    .build();

// 构建完整配置
auto workerConfig = WorkerConfigBuilder()
    .setFileConfig(
        FileConfigBuilder()
            .setFilePath("/path/to/video.h264")
            .build()
    )
    .setOutputConfig(
        OutputConfigBuilder()
            .setResolution(640, 480)
            .setBitsPerPixel(32)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useH264TacoWith(tacoConfig)  // 🎯 使用自定义 taco 配置
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
    .build();

VideoProductionLine pipeline;
pipeline.start(workerConfig, true, 2);
```

---

## 🧪 使用场景二：测试代码直接创建 Worker

### 方式 1：通过工厂创建

```cpp
#include "productionline/worker/BufferFillingWorkerFactory.hpp"

// 构建配置
auto workerConfig = WorkerConfigBuilder()
    .setFileConfig(
        FileConfigBuilder()
            .setFilePath("/path/to/test_video.mp4")
            .build()
    )
    .setOutputConfig(
        OutputConfigBuilder()
            .setResolution(1920, 1080)
            .setBitsPerPixel(32)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useH264Taco()
            .build()
    )
    .build();

// 通过工厂创建（配置在创建时注入）
auto worker = BufferFillingWorkerFactory::create(
    WorkerType::FFMPEG_VIDEO_FILE,
    workerConfig
);

// Worker 已配置完毕，直接使用
worker->open(workerConfig.file.file_path, 
             workerConfig.output.width, 
             workerConfig.output.height, 
             workerConfig.output.bits_per_pixel);
```

### 方式 2：通过 Facade 创建

```cpp
#include "productionline/worker/BufferFillingWorkerFacade.hpp"

// 构建配置
auto workerConfig = WorkerConfigBuilder()
    .setFileConfig(
        FileConfigBuilder()
            .setFilePath("/path/to/video.mp4")
            .build()
    )
    .setOutputConfig(
        OutputConfigBuilder()
            .setResolution(1920, 1080)
            .setBitsPerPixel(32)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useH264Taco()
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
    .build();

// 创建 Facade（配置在创建时应用）
BufferFillingWorkerFacade worker(
    workerConfig.worker_type,
    workerConfig
);

// 打开视频
worker.open(workerConfig.file.file_path,
            workerConfig.output.width,
            workerConfig.output.height,
            workerConfig.output.bits_per_pixel);
```

---

## 🔄 解码器配置对比表

| 解码器名称 | 使用场景 | 性能 | 兼容性 | 配置方式 |
|-----------|---------|------|--------|---------|
| 默认（nullptr） | 通用场景 | 🟡 中等 | 🟢 最佳 | `DecoderConfigBuilder().build()` |
| `"h264_taco"` | TACO 硬件平台 | 🟢 最高 | 🟡 仅TACO | `DecoderConfigBuilder().useH264Taco().build()` |
| `"h264_cuvid"` | NVIDIA GPU | 🟢 高 | 🟡 仅NVIDIA | `DecoderConfigBuilder().useH264Cuvid().build()` |
| `"h264_qsv"` | Intel Quick Sync | 🟢 高 | 🟡 仅Intel | `DecoderConfigBuilder().useH264Qsv().build()` |
| 软件解码 | 通用软件解码 | 🔴 低 | 🟢 最佳 | `DecoderConfigBuilder().useSoftware().build()` |

---

## 📝 完整示例

### 示例 1：命令行工具

```cpp
#include "productionline/VideoProductionLine.hpp"
#include "productionline/worker/WorkerConfig.hpp"
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <video_file> [--decoder <name>]\n", argv[0]);
        return 1;
    }
    
    const char* video_path = argv[1];
    const char* decoder_name = nullptr;
    
    // 解析命令行参数
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--decoder") == 0 && i + 1 < argc) {
            decoder_name = argv[i + 1];
            break;
        }
    }
    
    // 构建配置
    DecoderConfigBuilder decoderBuilder;
    if (decoder_name) {
        if (strcmp(decoder_name, "taco") == 0) {
            decoderBuilder.useH264Taco();
        } else if (strcmp(decoder_name, "cuda") == 0) {
            decoderBuilder.useH264Cuvid();
        } else if (strcmp(decoder_name, "qsv") == 0) {
            decoderBuilder.useH264Qsv();
        } else {
            decoderBuilder.setDecoderName(decoder_name);
        }
    }
    
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath(video_path)
                .build()
        )
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(decoderBuilder.build())
        .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
        .build();
    
    // 启动生产线
    VideoProductionLine pipeline;
    if (!pipeline.start(workerConfig, true, 4)) {
        printf("Failed to start pipeline\n");
        return 1;
    }
    
    // 运行...
    printf("Pipeline started successfully\n");
    
    return 0;
}

// 使用示例：
// ./app video.mp4
// ./app video.mp4 --decoder taco
// ./app video.mp4 --decoder cuda
```

### 示例 2：单元测试

```cpp
#include <gtest/gtest.h>
#include "productionline/worker/BufferFillingWorkerFactory.hpp"
#include "productionline/worker/WorkerConfig.hpp"

TEST(FfmpegWorkerTest, DecodeWithH264Taco) {
    // 构建配置
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath("test_data/test_video.mp4")
                .build()
        )
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useH264Taco()
                .build()
        )
        .build();
    
    // 创建 Worker
    auto worker = BufferFillingWorkerFactory::create(
        WorkerType::FFMPEG_VIDEO_FILE,
        workerConfig
    );
    
    // 测试
    ASSERT_TRUE(worker->open(workerConfig.file.file_path,
                            workerConfig.output.width,
                            workerConfig.output.height,
                            workerConfig.output.bits_per_pixel));
    EXPECT_GT(worker->getTotalFrames(), 0);
}

TEST(FfmpegWorkerTest, DecodeWithCuda) {
    auto workerConfig = WorkerConfigBuilder()
        .setFileConfig(
            FileConfigBuilder()
                .setFilePath("test_data/test_video.h264")
                .build()
        )
        .setOutputConfig(
            OutputConfigBuilder()
                .setResolution(1920, 1080)
                .setBitsPerPixel(32)
                .build()
        )
        .setDecoderConfig(
            DecoderConfigBuilder()
                .useH264Cuvid()
                .build()
        )
        .build();
    
    auto worker = BufferFillingWorkerFactory::create(
        WorkerType::FFMPEG_VIDEO_FILE,
        workerConfig
    );
    
    ASSERT_TRUE(worker->open(workerConfig.file.file_path,
                            workerConfig.output.width,
                            workerConfig.output.height,
                            workerConfig.output.bits_per_pixel));
}
```

### 示例 3：配置文件驱动

```cpp
#include <nlohmann/json.hpp>
#include <fstream>

// config.json
{
    "file": {
        "path": "/data/video.mp4",
        "start_frame": 0,
        "end_frame": 1000
    },
    "output": {
        "width": 1920,
        "height": 1080,
        "bits_per_pixel": 32
    },
    "decoder": {
        "name": "h264_taco",
        "enable_hardware": true
    },
    "production": {
        "loop": true,
        "thread_count": 4
    }
}

// main.cpp
std::ifstream config_file("config.json");
nlohmann::json j;
config_file >> j;

// 构建配置
auto workerConfig = WorkerConfigBuilder()
    .setFileConfig(
        FileConfigBuilder()
            .setFilePath(j["file"]["path"].get<std::string>().c_str())
            .setFrameRange(j["file"]["start_frame"], j["file"]["end_frame"])
            .build()
    )
    .setOutputConfig(
        OutputConfigBuilder()
            .setResolution(j["output"]["width"], j["output"]["height"])
            .setBitsPerPixel(j["output"]["bits_per_pixel"])
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .setDecoderName(j["decoder"]["name"].get<std::string>().c_str())
            .enableHardware(j["decoder"]["enable_hardware"])
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
    .build();

// 启动生产线
VideoProductionLine pipeline;
pipeline.start(workerConfig, 
               j["production"]["loop"], 
               j["production"]["thread_count"]);
```

---

## ⚙️ 技术实现细节

### 架构流程

```
用户代码
   ↓
WorkerConfigBuilder()
   .setFileConfig(...)
   .setOutputConfig(...)
   .setDecoderConfig(...)
   .build()
   ↓
VideoProductionLine.start(workerConfig, loop, thread_count)
   ↓
BufferFillingWorkerFacade(worker_type, workerConfig)
   ↓
BufferFillingWorkerFactory::create(type, workerConfig)
   ↓
FfmpegDecodeVideoFileWorker(workerConfig)
   ↓
Worker 创建完成（配置已应用）
```

### 关键优势

1. **配置归位**：所有 Worker 配置统一在 `WorkerConfig` 中
2. **职责清晰**：每个 Builder 只负责自己层级的配置
3. **API 简化**：生产线参数直接传递，无需复杂的 Config 结构体
4. **类型安全**：编译期检查，避免运行时错误
5. **易于扩展**：新增配置项不影响现有代码

---

## 🎓 v2.3 重构总结

### 重构前（v2.2）

```cpp
// ❌ 配置分散，职责混乱
VideoProductionLine::Config config;
config.file_path = "video.mp4";
config.width = 1920;
config.height = 1080;
config.bits_per_pixel = 32;
config.loop = true;
config.thread_count = 4;
config.worker_type = BufferFillingWorkerFactory::WorkerType::FFMPEG_VIDEO_FILE;

// Worker 配置单独设置
config.worker_config = WorkerConfigBuilder()
    .useH264TacoPreset()  // ❌ 跨层级方法
    .build();

pipeline.start(config);
```

### 重构后（v2.3）

```cpp
// ✅ 配置归位，职责清晰
auto workerConfig = WorkerConfigBuilder()
    .setFileConfig(
        FileConfigBuilder()
            .setFilePath("video.mp4")
            .build()
    )
    .setOutputConfig(
        OutputConfigBuilder()
            .setResolution(1920, 1080)
            .setBitsPerPixel(32)
            .build()
    )
    .setDecoderConfig(
        DecoderConfigBuilder()
            .useH264Taco()  // ✅ 在自己的 Builder 中
            .build()
    )
    .setWorkerType(WorkerType::FFMPEG_VIDEO_FILE)
    .build();

// ✅ 参数直接传递
pipeline.start(workerConfig, true, 4);
```

### 改进点

✅ **配置归位**：文件、输出配置迁移到 `WorkerConfig`  
✅ **Builder 职责清晰**：`WorkerConfigBuilder` 只负责组装  
✅ **API 简化**：`loop` 和 `thread_count` 作为函数参数  
✅ **避免循环依赖**：`WorkerType` 枚举独立定义  
✅ **易于测试**：配置对象可以独立创建和验证

---

## 📚 相关文档

- [WorkerConfig 重构总结](../../../../../CONFIG_REFACTORING_SUMMARY.md)
- [Worker 子系统架构](../Worker-subsystem-architecture.md)
- [VideoProductionLine 使用指南](../../VideoProductionLine.hpp)
- [综合架构文档](../../../../ARCHITECTURE.md)

---

## ✅ 总结

通过 **v2.3 配置系统重构**，我们实现了：

✅ **配置归位**：所有 Worker 配置统一在 `WorkerConfig` 中  
✅ **职责清晰**：每个 Builder 只负责自己层级的配置  
✅ **API 简化**：生产线参数直接传递，使用更直观  
✅ **类型安全**：编译期检查，避免运行时错误  
✅ **易于扩展**：新增配置项不影响现有代码  
✅ **灵活配置**：支持预设和自定义两种方式

这是一个符合大厂设计标准的完美解决方案！ 🎉
