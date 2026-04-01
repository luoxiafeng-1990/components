# BufferComparator 使用指南

## 📋 概述

`BufferComparator` 是一个用于对比验证两个解码器输出的工具类，头文件与实现位于 `include/consumptionline/`、`source/consumptionline/`，与 `BufferWriter` 并列；请直接 `#include "consumptionline/BufferComparator.hpp"`。

### 主要功能

- ✅ **格式自适应**：自动检测YUV/RGB格式并选择最优对比算法
- ✅ **2层验证**：快速验证（PSNR-Y/G）→ 深度验证（PSNR-YUV/RGB）
- ✅ **多格式支持**：YUV420P、NV12、ARGB、RGB24等
- ✅ **详细报告**：生成文本报告，包含失败帧详情
- ✅ **感知加权**：Y/G通道权重更高，符合人眼感知

---

## 🚀 快速开始

### 1. 包含头文件

```cpp
#include "consumptionline/BufferComparator.hpp"

using namespace consumptionline::io;
```

### 2. 基础使用示例

```cpp
// 1. 创建对比器
BufferComparator comparator;

// 2. 配置
CompareConfig config;
config.enable_psnr = true;                      // 启用 PSNR 计算
config.strategy = CompareConfig::AUTO_LAYERED;  // 自动分层
config.format_strategy = CompareConfig::AUTO;   // 格式自适应
config.min_psnr = 38.0;                         // >= 38dB 通过

// 3. 打开
comparator.open(config);

// 4. 对比循环
while (running) {
    Buffer* sw_buf = sw_pool->acquireFilled(...);
    Buffer* hw_buf = hw_pool->acquireFilled(...);
    
    FrameCompareResult result = comparator.compare(sw_buf, hw_buf);
    
    if (!result.passed) {
        LOG_WARN_FMT("Frame %d: PSNR=%.2f dB", 
                     result.frame_index, result.psnr_y);
    }
    
    sw_pool->releaseFilled(sw_buf);
    hw_pool->releaseFilled(hw_buf);
}

// 5. 关闭并打印结果
comparator.close();
comparator.printSummary();

// 6. 检查是否全部通过
if (comparator.isPassed()) {
    LOG_INFO("✅ All tests passed");
} else {
    LOG_ERROR("❌ Some tests failed");
}
```

---

## ⚙️ 配置选项

### CompareConfig 结构体

> **注意**：`CompareConfig` 现在统一定义在 `WorkerConfig::ConsumerTypeConfig::CompareType` 中，
> `consumptionline::io::CompareConfig` 是其类型别名。

```cpp
struct CompareType {  // 或 using CompareConfig = CompareType
    // ========== 指标开关 ==========
    bool enable_psnr = false;           // 是否启用 PSNR 计算
    bool enable_ssim = false;           // 是否启用 SSIM 计算
    
    // ========== 验证策略 ==========
    Strategy strategy = AUTO_LAYERED;
    // - FAST_ONLY:      仅快速验证（每帧PSNR-Y/G）
    // - AUTO_LAYERED:   自动分层（推荐）⭐
    // - DEEP_ALWAYS:    总是深度验证（慢但详细）
    
    // ========== 格式处理 ==========
    FormatStrategy format_strategy = AUTO;
    // - AUTO:       自动检测并选择最优策略（推荐）⭐
    // - FORCE_YUV:  强制转换到YUV空间对比
    // - FORCE_RGB:  强制转换到RGB空间对比
    // - NATIVE:     原生格式对比（要求两边格式一致）
    
    // ========== 阈值配置 ==========
    double min_psnr = 38.0;             // >= 38dB 快速通过
    double warn_psnr = 35.0;            // < 35dB 触发深度验证
    double min_ssim = 0.95;             // >= 0.95 认为质量优秀
    double warn_ssim = 0.90;            // < 0.90 触发警告
    
    // ========== 输出选项 ==========
    bool verbose = false;               // 详细日志
    bool save_report = false;           // 保存报告到文件
    std::string report_path = "./decoder_compare_report.txt";
};
```

---

## 📊 PSNR 阈值参考（业界标准）

| PSNR范围 | 质量评价 | 人眼感知 | 判定 |
|---------|---------|---------|------|
| >= 45 dB | 优秀 | 完全看不出差异 | ✅ PASS |
| 40-45 dB | 非常好 | 几乎看不出差异 | ✅ PASS |
| **38-40 dB** | **良好** | **仔细看有细微差异** | ✅ **PASS**（推荐阈值） |
| 35-38 dB | 可接受 | 存在轻微差异 | ⚠️ WARN |
| 30-35 dB | 较差 | 明显可见失真 | ❌ FAIL |
| < 30 dB | 很差 | 严重失真 | ❌ FAIL |

---

## 🎨 多格式支持

### 格式自适应策略

```
输入格式组合          → 自动选择策略
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
NV12 vs NV12         → YUV直接对比（最快）
ARGB vs ARGB         → RGB直接对比
NV12 vs YUV420P      → YUV统一对比
ARGB vs RGB24        → RGB统一对比
NV12 vs ARGB         → 转换到YUV420P对比（标准）
```

### YUV格式对比

**支持格式：** YUV420P, NV12, NV21, YUV422P, YUV444P, P010LE

**对比方法：**
- Y平面：完整PSNR（亮度，权重×4）
- U平面：完整PSNR（色度）
- V平面：完整PSNR（色度）
- 加权平均：`(Y×4 + U + V) / 6`

### RGB格式对比

**支持格式：** ARGB, ABGR, BGRA, RGBA, RGB24, BGR24, RGB48LE

**对比方法：**
- R通道：PSNR
- G通道：PSNR（权重×2，人眼对绿色最敏感）
- B通道：PSNR
- 加权平均：`(R + G×2 + B) / 4`

---

## 📈 2层验证流程

```
┌──────────────────────────────────────────────────┐
│ 层1：快速验证（每帧都执行）                        │
├──────────────────────────────────────────────────┤
│ 1. 元数据检查（分辨率、格式）                      │
│ 2. 计算主要平面/通道PSNR（Y/G）                   │
│ 3. 判定：                                         │
│    - PSNR >= 38dB → ✅ 快速通过（跳过层2）        │
│    - PSNR < 38dB  → 触发层2                       │
└──────────────────────────────────────────────────┘
                        ↓
┌──────────────────────────────────────────────────┐
│ 层2：深度验证（仅失败/边界时执行）                 │
├──────────────────────────────────────────────────┤
│ 1. 计算完整PSNR（YUV三平面 或 RGB三通道）          │
│ 2. 像素差异统计（可选）                           │
│ 3. 加权平均                                       │
│ 4. 综合判定：                                     │
│    - PSNR >= 38dB → ✅ PASS                       │
│    - 35dB <= PSNR < 38dB → ⚠️ WARN               │
│    - PSNR < 35dB → ❌ FAIL                        │
│ 5. 保存失败帧详情到报告                           │
└──────────────────────────────────────────────────┘

性能优势：95%的帧快速通过，比全量深度验证快4倍 ⚡
```

---

## 📝 测试用例

### 运行测试

```bash
# 对比本地视频文件（硬件 vs 软件解码）
./display_test -m decoder_compare /path/to/video.mp4

# 对比RTSP流
./display_test -m decoder_compare rtsp://192.168.1.100:554/stream

# 查看所有测试用例
./display_test -l
```

### 测试输出示例

```
╔═══════════════════════════════════════════════════════╗
║   Test: Decoder Comparison (Hardware vs Software)    ║
╚═══════════════════════════════════════════════════════╝

[Step 1] Configuring MultiWorkerProductionLine...
  ✅ Hardware Decoder: h264_taco
  ✅ Software Decoder: libavcodec

[Step 2] Starting MultiWorkerProductionLine...

[Step 3] Getting BufferPools...
  Hardware BufferPool: 'FfmpegDecodeVideoFileWorker_/path/to/video.mp4'
  Software BufferPool: 'FfmpegDecodeVideoFileWorker_/path/to/video.mp4'

[Step 4] Creating BufferComparator...
  ✅ BufferComparator initialized
  Strategy: AUTO_LAYERED (fast → deep)
  Format: AUTO (YUV/RGB adaptive)
  PSNR threshold: 38.0 dB (pass), 35.0 dB (warn)

[Step 5] Comparing decoder outputs...
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Progress: 50/300 frames (passed: 48, warned: 2, failed: 0)
  Progress: 100/300 frames (passed: 97, warned: 3, failed: 0)
  ...
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

═══════════════════════════════════════════════════════
  Decoder Comparison Results
═══════════════════════════════════════════════════════
╔═══════════════════════════════════════════════════════╗
║  BufferComparator Summary                              ║
╚═══════════════════════════════════════════════════════╝
  Total frames compared: 300
  Passed: 295 ✅ (98.3%)
  Warned: 5 ⚠️  (1.7%)
  Failed: 0 ❌ (0.0%)

  PSNR Statistics:
    Average: Y=42.31 U=43.12 V=43.05 dB
    Range:   Y=[36.82, 48.91] dB

  ✅ Result: ALL TESTS PASSED
╚═══════════════════════════════════════════════════════╝

💡 Detailed report saved to: ./decoder_compare_report.txt
```

---

## 📄 报告格式

生成的报告文件示例：

```
═══════════════════════════════════════════════════════
  Decoder Comparison Report
═══════════════════════════════════════════════════════
Strategy: AUTO_LAYERED
Format Strategy: AUTO
PSNR Threshold: Pass >= 38.0 dB, Warn >= 35.0 dB
═══════════════════════════════════════════════════════

Frame 23: WARN ⚠️
  Formats: nv12 vs nv12
  PSNR: Y=37.21 U=42.31 V=41.89 dB (avg=38.91 dB)

Frame 156: WARN ⚠️
  Formats: nv12 vs nv12
  PSNR: Y=36.82 U=41.05 V=40.78 dB (avg=38.32 dB)

═══════════════════════════════════════════════════════
  Summary
═══════════════════════════════════════════════════════
Total frames compared: 300
Passed: 295 (98.3%)
Warned: 5 (1.7%)
Failed: 0 (0.0%)

PSNR Statistics:
  Average: Y=42.31 U=43.12 V=43.05 dB
  Min Y: 36.82 dB
  Max Y: 48.91 dB

✅ ALL TESTS PASSED
═══════════════════════════════════════════════════════
```

---

## 🔧 高级用法

### 1. 不同配置场景

```cpp
// 场景A：CI/CD快速测试（只关心是否通过）
CompareConfig config;
config.enable_psnr = true;
config.strategy = CompareConfig::FAST_ONLY;
config.min_psnr = 40.0;              // 更严格
config.verbose = false;              // 减少日志

// 场景B：深度调试（每帧都详细分析）
CompareConfig config;
config.strategy = CompareConfig::DEEP_ALWAYS;
config.verbose = true;
config.save_report = true;

// 场景C：RGB格式强制对比
CompareConfig config;
config.format_strategy = CompareConfig::FORCE_RGB;
```

### 2. 实时判定

```cpp
while (running) {
    Buffer* sw_buf = sw_pool->acquireFilled(...);
    Buffer* hw_buf = hw_pool->acquireFilled(...);
    
    FrameCompareResult result = comparator.compare(sw_buf, hw_buf);
    
    // 实时判定
    switch (result.level) {
        case FrameCompareResult::PASS:
            // 继续
            break;
            
        case FrameCompareResult::WARN:
            LOG_WARN_FMT("Frame %d borderline: PSNR=%.2f dB",
                         result.frame_index, result.psnr_y);
            break;
            
        case FrameCompareResult::FAIL:
            LOG_ERROR_FMT("Frame %d failed: PSNR=%.2f dB",
                          result.frame_index, result.psnr_y);
            // 可选：停止测试
            break;
    }
    
    sw_pool->releaseFilled(sw_buf);
    hw_pool->releaseFilled(hw_buf);
}
```

---

## 🎓 PSNR 基础知识

### 什么是PSNR？

PSNR (Peak Signal-to-Noise Ratio) 是衡量图像质量的客观指标，单位为dB（分贝）。

### 计算公式

```
MSE = 平均平方误差 = Σ(pixel1 - pixel2)² / 像素总数
PSNR = 10 × log₁₀(255² / MSE)
```

### PSNR值含义

- **PSNR越高** = 图像越相似 = 质量越好
- **PSNR = 100 dB** = 完全一致（MSE = 0）
- **PSNR ≥ 40 dB** = 业界认为"视觉无损"
- **PSNR < 30 dB** = 肉眼可见明显失真

---

## ⚠️ 注意事项

1. **格式要求**：
   - 两个Buffer必须有相同的分辨率
   - Buffer必须包含图像元数据（`hasImageMetadata() = true`）

2. **性能建议**：
   - 优先使用 `AUTO_LAYERED` 策略（比 `DEEP_ALWAYS` 快4倍）
   - 相同格式对比最快，避免不必要的格式转换

3. **阈值设置**：
   - 有损编解码（H.264）：38dB 是合理的通过阈值
   - 无损编解码：可以设置更高（45dB）

4. **混合格式**：
   - YUV vs RGB 对比会自动转换到YUV空间
   - 有性能开销，尽量保持两边格式一致

---

## 📚 参考资料

- [PSNR Wikipedia](https://en.wikipedia.org/wiki/Peak_signal-to-noise_ratio)
- [ITU-T Rec. BT.601](https://www.itu.int/rec/R-REC-BT.601/) - YUV↔RGB 转换标准
- [FFmpeg Pixel Formats](https://ffmpeg.org/doxygen/trunk/pixfmt_8h.html)

---

## 🐛 故障排查

### 问题1：报错 "Format mismatch in NATIVE mode"

**原因**：使用了 `NATIVE` 模式但两个解码器输出格式不同。

**解决**：改用 `AUTO` 模式：
```cpp
config.format_strategy = CompareConfig::AUTO;
```

### 问题2：PSNR异常低（<20dB）

**可能原因**：
1. 两个解码器解码的不是同一帧（同步问题）
2. 分辨率不匹配
3. 解码器配置错误

**解决**：
1. 检查 `MultiWorkerProductionLine` 配置
2. 确保两个Worker使用相同的视频源
3. 查看日志中的格式信息

### 问题3：对比速度很慢

**原因**：使用了 `DEEP_ALWAYS` 或格式转换。

**解决**：
1. 改用 `AUTO_LAYERED` 策略
2. 确保两个解码器输出相同格式
3. 减少对比帧数（如只对比前100帧）

---

## 📞 支持

如有问题，请查看：
- 项目架构文档：`packages/components/ARCHITECTURE.md`
- BufferWriter文档：`packages/components/include/consumptionline/BufferWriter.hpp`
- 测试用例代码：`packages/components/test_cases/dec/test.cpp` (test_decoder_compare)
