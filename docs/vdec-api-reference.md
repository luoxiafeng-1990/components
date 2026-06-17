# VdecPlugin API Reference

> 视频解码测试插件，实现 `IOptionPlugin` 接口，支持 H.264/H.265/MJPEG 硬件与软件解码、RTSP 流解码及 PSNR/SSIM 质量验证。

| 属性 | 值 |
| --- | --- |
| 头文件 | `test_cases/vdec/VdecPlugin.hpp` |
| 源文件 | `test_cases/vdec/VdecPlugin.cpp` |
| 命名空间 | `test::vdec` |
| 版本 | 5.0 — 重构为 IOptionPlugin 插件架构 |
| 依赖 | `IOptionPlugin.hpp`, `ExecuteMode.hpp`, `DataSourceOptions.hpp`, `BufferConsumerService.hpp`, `WorkerConfig.hpp` |

---

## 目录

- [1. 概述](#1-概述)
- [2. 类型定义](#2-类型定义)
- [3. 结构体](#3-结构体)
  - [3.1 DecodeTestParams](#31-decodetestparams)
- [4. 类](#4-类)
  - [4.1 IOptionPlugin（基类接口）](#41-ioptionplugin基类接口)
  - [4.2 VdecPlugin](#42-vdecplugin)
- [5. 预定义测试集](#5-预定义测试集)

---

## 1. 概述

VdecPlugin 是视频解码模块的测试插件，封装了所有解码相关的测试功能：

- **H.264/H.265/MJPEG** 硬件解码（TACO 芯片加速）
- **软件解码** 对比基准
- **RTSP 流解码**（CBR/VBR 码率控制）
- **PSNR/SSIM** 质量验证（hw vs sw 对比）

### 架构位置

```
main() → CLI::App → VdecPlugin::registerOptions()
                   → CLI11 parse
                   → VdecPlugin::applyTo(shared_config)
                   → VdecPlugin::buildPipelineConfigs()
                   → ExecuteMode::single() / compare() / parallel()
```

### 执行模式映射

| 模式 | 触发条件 | 说明 |
| --- | --- | --- |
| SINGLE | 默认（无特殊标志） | 单路解码 |
| COMPARE | `--psnr` 或 `--ssim` | HW vs SW 对比，计算质量指标 |
| PARALLEL | `--threads N` 或预定义 `parallel` 测试 | 多线程/多 Worker 并发解码 |

### 使用示例

```bash
./qa_cases vdec --file video.mp4 --codec h264 --width 1920 --height 1080
./qa_cases vdec --rtsp rtsp://192.168.1.100/stream
./qa_cases vdec --psnr video.mp4          # COMPARE 模式
./qa_cases vdec --threads 4 video.mp4     # PARALLEL 模式
./qa_cases vdec h264_1920x1080_30 video.mp4  # 预定义测试
./qa_cases vdec -l                        # 列出所有预定义测试
```

### 依赖关系

| 依赖头文件 | 提供的关键类型/功能 |
| --- | --- |
| `IOptionPlugin.hpp` | `IOptionPlugin` 基类接口 |
| `ExecuteMode.hpp` | `ExecuteMode` 静态执行方法（single/compare/parallel） |
| `DataSourceOptions.hpp` | `DataSourceOptions` 数据源横切选项 |
| `BufferConsumerService.hpp` | `consumer::ConsumeResult` 测试结果类型 |
| `WorkerConfig.hpp` | `WorkerConfig` 管线配置结构体 |
| `WorkerConfigFactory.hpp` | `WorkerConfigFactory` 配置工厂方法 |

---

## 2. 类型定义

| 别名 | 原始类型 | 说明 |
| --- | --- | --- |
| `TestResult` | `consumer::ConsumeResult` | 解码测试结果类型，封装测试执行的成功/失败及指标数据 |

---

## 3. 结构体

### 3.1 DecodeTestParams

> 解码测试参数集合，描述单个解码测试的完整配置。

**头文件**：`test_cases/vdec/VdecPlugin.hpp`
**命名空间**：`test::vdec`

#### 成员变量

> 按声明顺序排列。

| 名称 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `codec` | `std::string` | `"h264"` | 编解码器名称（`h264`, `h265`, `mjpeg`） |
| `width` | `int` | `1920` | 视频分辨率宽度（像素） |
| `height` | `int` | `1080` | 视频分辨率高度（像素） |
| `fps` | `double` | `30.0` | 目标帧率 |
| `profile` | `std::string` | `"main"` | 编码 profile（`main`, `baseline`, `high`, `cbr`, `vbr`, `parallel`） |
| `use_hardware` | `bool` | `true` | 是否使用硬件解码（`false` 时使用 FFmpeg 软件解码） |
| `predefined_name` | `std::string` | `""` | 匹配的预定义测试名称（空字符串表示非预定义测试） |

#### 构造函数

| 签名 | 说明 |
| --- | --- |
| `DecodeTestParams(c, w, h, f, p, hw)` | 全参构造，所有参数均有默认值 |

**参数详情**：

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `c` | `const std::string&` | `"h264"` | 编解码器 |
| `w` | `int` | `1920` | 宽度 |
| `h` | `int` | `1080` | 高度 |
| `f` | `double` | `30.0` | 帧率 |
| `p` | `const std::string&` | `"main"` | profile |
| `hw` | `bool` | `true` | 是否硬件解码 |

#### 成员函数

| 函数类别 | 函数签名 | 返回类型 | 说明 |
| --- | --- | --- | --- |
| 普通函数 | `isPredefined() const` | `bool` | 判断是否使用了预定义测试（`predefined_name` 非空） |

---

## 4. 类

### 4.1 IOptionPlugin（基类接口）

> 命令行选项插件接口（CLI11 版本）。所有功能模块（vdec、pp、record、writer、display、npu 等）均实现此接口，通过 CLI11 注册选项、解析参数、构建 WorkerConfig。

**头文件**：`test_cases/common/IOptionPlugin.hpp`
**命名空间**：`test`
**版本**：7.0 — 从 getopt_long 迁移到 CLI11

#### 架构说明

1. `main()` 创建 `CLI::App` 及子命令
2. 每个插件通过 `registerOptions()` 将选项注册到对应子命令
3. CLI11 统一解析后，选项值自动填充到插件成员变量
4. 插件通过 `applyTo()` 注入配置，`buildPipelineConfigs()` 构建执行管线

#### 成员函数

> 按函数类别排序：构造/析构 → 纯虚函数 → 虚函数。所有成员均为 public，省略访问级别列。

| 函数类别 | 函数签名 | 返回类型 | 说明 |
| --- | --- | --- | --- |
| 构造/析构 | `~IOptionPlugin() = default` | — | 虚析构函数 |
| 纯虚函数 | `getName() const = 0` | `std::string` | 获取插件名称（如 `"vdec"`） |
| 纯虚函数 | `registerOptions(app) = 0` | `void` | 向 CLI::App 子命令注册本插件的命令行选项 |
| 纯虚函数 | `applyTo(config) const = 0` | `void` | 将解析到的参数注入共享 WorkerConfig |
| 虚函数 | `getDescription() const` | `std::string` | 获取插件描述（默认返回空字符串） |
| 虚函数 | `listTests() const` | `void` | 列出可用的预定义测试（默认空实现） |
| 虚函数 | `buildPipelineConfigs(shared_config)` | `std::vector<WorkerConfig>` | 构建管线配置列表（默认返回空） |
| 虚函数 | `getTestName() const` | `std::string` | 获取当前测试名称（默认返回空字符串） |
| 虚函数 | `handlePreActions()` | `int` | 解析后的预处理动作（默认返回 -1 继续执行） |

#### 函数详情

---

**`registerOptions`**

```cpp
virtual void registerOptions(CLI::App& app) = 0;
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `app` | `CLI::App&` | in/out | CLI11 子命令对象，选项注册到此处 |

**说明**：CLI11 解析完成后，注册时绑定的成员变量自动填充。不同子命令有独立的选项命名空间，不会冲突。

---

**`applyTo`**

```cpp
virtual void applyTo(WorkerConfig& config) const = 0;
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `config` | `WorkerConfig&` | in/out | 共享配置对象，插件将解析到的参数注入其中 |

---

**`buildPipelineConfigs`**

```cpp
virtual std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `shared_config` | `const WorkerConfig&` | in | 由各插件 `applyTo()` 后的共享配置 |

**返回值**：`std::vector<WorkerConfig>`

| 返回数量 | 含义 |
| --- | --- |
| 空 | 本插件无可执行的工作 |
| 1 个 | SINGLE 模式 |
| 2 个 | COMPARE 模式（hw vs sw） |
| N 个 | PARALLEL / BATCH 模式 |

---

**`handlePreActions`**

```cpp
virtual int handlePreActions();
```

**返回值**：`int`

| 值 | 含义 |
| --- | --- |
| `>= 0` | 应退出程序，返回值为退出码 |
| `-1` | 继续执行后续流程 |

---

### 4.2 VdecPlugin

> 视频解码测试插件。实现 `IOptionPlugin` 接口，作为主执行插件，通过 `ExecuteMode` 静态类驱动 SINGLE / COMPARE / PARALLEL 三种执行模式。

**头文件**：`test_cases/vdec/VdecPlugin.hpp`
**源文件**：`test_cases/vdec/VdecPlugin.cpp`
**命名空间**：`test::vdec`
**继承**：`public IOptionPlugin`

#### 成员函数

> 按函数类别排序：构造/析构 → override → 静态函数 → 普通函数。同类别内按 public → private 排序。

| 函数类别 | 访问级别 | 函数签名 | 返回类型 | 说明 |
| --- | --- | --- | --- | --- |
| 构造/析构 | public | `VdecPlugin() = default` | — | 默认构造 |
| 构造/析构 | public | `~VdecPlugin() override = default` | — | 默认析构 |
| override | public | `getName() const` | `std::string` | 返回 `"vdec"` |
| override | public | `getDescription() const` | `std::string` | 返回 `"视频解码测试"` |
| override | public | `registerOptions(app)` | `void` | 注册全部解码相关 CLI 选项（见命令行选项表） |
| override | public | `applyTo(config) const` | `void` | 注入数据源、帧数限制、compare 阈值等到共享配置 |
| override | public | `listTests() const` | `void` | 打印 47 个预定义测试的分类列表 |
| override | public | `handlePreActions()` | `int` | 解析 decoder/resolution 字符串、匹配预定义测试、校验输入 |
| override | public | `buildPipelineConfigs(shared)` | `std::vector<WorkerConfig>` | 根据模式构建 WorkerConfig 列表（见执行模式映射） |
| override | public | `getTestName() const` | `std::string` | 生成格式化测试名（预定义或 Custom） |
| 静态函数 | public | `getPredefinedTests()` | `const std::map<std::string, DecodeTestParams>&` | 获取全部 47 个预定义测试参数表 |
| 普通函数 | private | `resolveParams() const` | `DecodeTestParams` | 合并 `threads_` 到 params 的 profile 字段 |

#### 函数详情

---

**`registerOptions`**

```cpp
void registerOptions(CLI::App& app) override;
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `app` | `CLI::App&` | in/out | CLI11 子命令对象 |

注册的命令行选项：

| 选项 | 短选项 | 绑定成员 | 类型 | 说明 |
| --- | --- | --- | --- | --- |
| `--list` | `-l` | `show_list_` | flag | 列出所有预定义测试 |
| `--file` | `-f` | `input_path_` | string | 视频文件路径 |
| `--rtsp` | `-r` | `input_path_` | string | RTSP URL |
| `--codec` | `-c` | `params_.codec` | string | 编解码格式（`h264`\|`h265`\|`mjpeg`\|…） |
| `--decoder` | `-D` | `decoder_str_` | string | 解码方式（`hw`\|`sw`，默认 hw） |
| `--width` | `-W` | `params_.width` | int | 分辨率宽度 |
| `--height` | `-H` | `params_.height` | int | 分辨率高度 |
| `--resolution` | `-R` | `resolution_str_` | string | 分辨率字符串（如 `1920x1080`） |
| `--fps` | `-F` | `params_.fps` | double | 目标帧率 |
| `--max-frames` | `-m` | `max_frames_` | int | 最大帧数（-1=无限制） |
| `--psnr` | `-p` | `enable_psnr_` | flag | 启用 PSNR 验证（触发 COMPARE 模式） |
| `--ssim` | `-S` | `enable_ssim_` | flag | 启用 SSIM 验证（触发 COMPARE 模式） |
| `--min-psnr` | `-P` | `min_psnr_` | double | PSNR 最低阈值（默认 30.0 dB） |
| `--min-ssim` | `-M` | `min_ssim_` | double | SSIM 最低阈值（默认 0.95） |
| `--verbose` | `-v` | `verbose_` | flag | 详细日志输出 |
| `--threads` | `-t` | `threads_` | int | 并发路数（触发 PARALLEL 模式） |
| `--loop` | — | `loop_` | flag | 循环播放 |
| _(positional)_ | — | `positional_args_` | vector | 测试名或输入文件路径 |

另外调用 `ds_opts_.registerTo(app)` 注册 DataSource 横切选项。

---

**`applyTo`**

```cpp
void applyTo(WorkerConfig& config) const override;
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `config` | `WorkerConfig&` | in/out | 共享配置 |

**注入内容**：
- 数据源路径（`input_path_`）、最大帧数（`max_frames_`）、循环标志（`loop_`）
- PSNR/SSIM 开关及阈值
- verbose 标志
- DataSource 横切选项

---

**`handlePreActions`**

```cpp
int handlePreActions() override;
```

**处理逻辑**：
1. 解析 `decoder_str_`：`"sw"` / `"software"` → 设置 `params_.use_hardware = false`
2. 解析 `resolution_str_`：如 `"1920x1080"` → 拆分为 width/height
3. 遍历 `positional_args_`：匹配预定义测试名 → 设置 params；否则作为 `input_path_`
4. 若 `show_list_` → 调用 `listTests()` 并返回 `0`
5. 若 `input_path_` 为空 → 报错并返回 `1`
6. 否则返回 `-1`（继续执行）

---

**`buildPipelineConfigs`**

```cpp
std::vector<WorkerConfig> buildPipelineConfigs(const WorkerConfig& shared_config) override;
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `shared_config` | `const WorkerConfig&` | in | 共享配置（已被各插件 applyTo） |

**构建逻辑**：

| 条件 | 线程数 | 每路生成 | 总输出 |
| --- | --- | --- | --- |
| `input_path_` 为空 | — | — | 空（无工作） |
| COMPARE（psnr 或 ssim 开启） | thread_count | hw + sw 两个 config | 2 × thread_count |
| PARALLEL（profile 含 `parallel`） | 从 profile 解析（默认 2） | 按 `use_hardware` 选择 | thread_count |
| 默认 SINGLE | 1 | 按 `use_hardware` 选择 | 1 |

内部使用 `WorkerConfigFactory::createDecode()` / `createSoftwareDecode()` 创建基础配置，然后叠加 consumer_type、target_fps、max_frames、loop 设置。

---

**`getTestName`**

```cpp
std::string getTestName() const override;
```

**返回格式**：
- 预定义测试：`"h264_1920x1080_30 (h264 1920x1080 30fps)"`
- 自定义输入：`"Custom: filename.mp4 (h264 1920x1080 30fps)"`

---

**`getPredefinedTests`**（静态）

```cpp
static const std::map<std::string, DecodeTestParams>& getPredefinedTests();
```

返回包含 47 个预定义测试的静态 map（详见 [§5 预定义测试集](#5-预定义测试集)）。

---

**`resolveParams`**（私有）

```cpp
DecodeTestParams resolveParams() const;
```

合并逻辑：若 `threads_ > 0`，将 `params_.profile` 覆盖为 `"parallel_{threads_}"`。

---

#### 成员变量

> 所有成员均为 private。按功能分组排列。

| 分组 | 名称 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| 控制标志 | `show_list_` | `bool` | `false` | 是否显示预定义测试列表（`-l`） |
| 核心参数 | `params_` | `DecodeTestParams` | _(默认构造)_ | 解码测试参数集 |
| 输入 | `input_path_` | `std::string` | `""` | 视频文件路径或 RTSP URL |
| 输入 | `positional_args_` | `std::vector<std::string>` | `{}` | CLI11 位置参数列表 |
| 解析中间量 | `decoder_str_` | `std::string` | `""` | 解码方式原始字符串（`hw`/`sw`） |
| 解析中间量 | `resolution_str_` | `std::string` | `""` | 分辨率原始字符串（如 `1920x1080`） |
| COMPARE | `enable_psnr_` | `bool` | `false` | 启用 PSNR 质量对比 |
| COMPARE | `enable_ssim_` | `bool` | `false` | 启用 SSIM 质量对比 |
| COMPARE | `min_psnr_` | `double` | `0.0` | PSNR 最低阈值（dB） |
| COMPARE | `min_ssim_` | `double` | `0.0` | SSIM 最低阈值 |
| 执行控制 | `verbose_` | `bool` | `false` | 详细日志输出 |
| 执行控制 | `threads_` | `int` | `0` | 并发路数（0=未指定，不启用 PARALLEL） |
| 执行控制 | `max_frames_` | `int` | `-1` | 最大解码帧数（-1=无限制） |
| 执行控制 | `loop_` | `bool` | `false` | 循环播放 |
| 横切选项 | `ds_opts_` | `DataSourceOptions` | _(默认构造)_ | DataSource 横切选项 |

---

## 5. 预定义测试集

> `getPredefinedTests()` 返回的静态 map，按编解码器类型 → 测试场景分类排序。共 47 个。

### 5.1 H.264 硬件解码（9 个）— SINGLE 模式

| 测试名 | 分辨率 | 帧率 | Profile |
| --- | --- | --- | --- |
| `h264_128x128_30` | 128×128 | 30 | main |
| `h264_320x240_30` | 320×240 | 30 | high |
| `h264_640x480_30` | 640×480 | 30 | main |
| `h264_640x480_60` | 640×480 | 60 | high |
| `h264_1280x720_30` | 1280×720 | 30 | high |
| `h264_1920x1080_30` | 1920×1080 | 30 | high |
| `h264_1920x1080_60` | 1920×1080 | 60 | high |
| `h264_2560x1440_30` | 2560×1440 | 30 | high |
| `h264_3840x2160_30` | 3840×2160 | 30 | high |

### 5.2 H.265/HEVC 硬件解码（9 个）— SINGLE 模式

| 测试名 | 分辨率 | 帧率 | Profile |
| --- | --- | --- | --- |
| `h265_128x128_30` | 128×128 | 30 | main |
| `h265_320x240_30` | 320×240 | 30 | main |
| `h265_640x480_30` | 640×480 | 30 | main |
| `h265_640x480_60` | 640×480 | 60 | main |
| `h265_1280x720_30` | 1280×720 | 30 | main |
| `h265_1920x1080_30` | 1920×1080 | 30 | main |
| `h265_1920x1080_60` | 1920×1080 | 60 | main |
| `h265_2560x1440_30` | 2560×1440 | 30 | main |
| `h265_3840x2160_30` | 3840×2160 | 30 | main |

### 5.3 MJPEG 硬件解码（9 个）— SINGLE 模式

| 测试名 | 分辨率 | 帧率 | Profile |
| --- | --- | --- | --- |
| `mjpeg_128x128_30` | 128×128 | 30 | — |
| `mjpeg_320x240_30` | 320×240 | 30 | — |
| `mjpeg_640x480_30` | 640×480 | 30 | — |
| `mjpeg_640x480_60` | 640×480 | 60 | — |
| `mjpeg_1280x720_30` | 1280×720 | 30 | — |
| `mjpeg_1920x1080_30` | 1920×1080 | 30 | — |
| `mjpeg_1920x1080_60` | 1920×1080 | 60 | — |
| `mjpeg_2560x1440_30` | 2560×1440 | 30 | — |
| `mjpeg_3840x2160_30` | 3840×2160 | 30 | — |

### 5.4 软件解码（2 个）— SINGLE 模式

| 测试名 | 编解码器 | 分辨率 | 帧率 | 硬件解码 |
| --- | --- | --- | --- | --- |
| `sw_h264_1920x1080_30` | h264 | 1920×1080 | 30 | ❌ |
| `sw_h265_1920x1080_30` | h265 | 1920×1080 | 30 | ❌ |

### 5.5 RTSP H.264（6 个）— SINGLE 模式

| 测试名 | 分辨率 | 帧率 | 码率控制 |
| --- | --- | --- | --- |
| `rtsp_h264_1280x720_30_cbr` | 1280×720 | 30 | CBR |
| `rtsp_h264_1280x720_30_vbr` | 1280×720 | 30 | VBR |
| `rtsp_h264_1920x1080_30_cbr` | 1920×1080 | 30 | CBR |
| `rtsp_h264_1920x1080_30_vbr` | 1920×1080 | 30 | VBR |
| `rtsp_h264_3840x2160_30_cbr` | 3840×2160 | 30 | CBR |
| `rtsp_h264_3840x2160_30_vbr` | 3840×2160 | 30 | VBR |

### 5.6 RTSP H.265（6 个）— SINGLE 模式

| 测试名 | 分辨率 | 帧率 | 码率控制 |
| --- | --- | --- | --- |
| `rtsp_h265_1280x720_30_cbr` | 1280×720 | 30 | CBR |
| `rtsp_h265_1280x720_30_vbr` | 1280×720 | 30 | VBR |
| `rtsp_h265_1920x1080_30_cbr` | 1920×1080 | 30 | CBR |
| `rtsp_h265_1920x1080_30_vbr` | 1920×1080 | 30 | VBR |
| `rtsp_h265_3840x2160_30_cbr` | 3840×2160 | 30 | CBR |
| `rtsp_h265_3840x2160_30_vbr` | 3840×2160 | 30 | VBR |

### 5.7 RTSP MJPEG（1 个）— SINGLE 模式

| 测试名 | 分辨率 | 帧率 |
| --- | --- | --- |
| `rtsp_mjpeg_32768x18432_30` | 32768×18432 | 30 |

### 5.8 多 Worker 并发（2 个）— PARALLEL 模式

| 测试名 | 编解码器 | 分辨率 | 帧率 | 说明 |
| --- | --- | --- | --- | --- |
| `multi_worker` | h264 | 1920×1080 | 30 | HW+SW 并发解码 |
| `multi_worker_4k` | h264 | 3840×2160 | 30 | HW+SW 并发 4K 解码 |

### 5.9 多线程解码（3 个）— PARALLEL 模式

| 测试名 | 编解码器 | 分辨率 | 帧率 | 线程数 |
| --- | --- | --- | --- | --- |
| `multithread_2` | h264 | 1920×1080 | 30 | 2 |
| `multithread_4` | h264 | 1920×1080 | 30 | 4 |
| `multithread_8` | h264 | 1920×1080 | 30 | 8 |

### 5.10 MP4 解码基础测试（12 个）— SINGLE 模式

> ZYW 新增，配合 PP 使用。

| 测试名 | 编解码器 | 分辨率 | 帧率 | Profile |
| --- | --- | --- | --- | --- |
| `mp4_h264` | h264 | 1920×1080 | 30 | high |
| `mp4_h264_720p` | h264 | 1280×720 | 30 | high |
| `mp4_h264_1080p` | h264 | 1920×1080 | 30 | high |
| `mp4_h264_4k` | h264 | 3840×2160 | 30 | high |
| `mp4_h265` | h265 | 1920×1080 | 30 | main |
| `mp4_h265_720p` | h265 | 1280×720 | 30 | main |
| `mp4_h265_1080p` | h265 | 1920×1080 | 30 | main |
| `mp4_h265_4k` | h265 | 3840×2160 | 30 | main |
| `mp4_mjpeg` | mjpeg | 1920×1080 | 30 | — |
| `mp4_mjpeg_720p` | mjpeg | 1280×720 | 30 | — |
| `mp4_mjpeg_1080p` | mjpeg | 1920×1080 | 30 | — |
| `mp4_mjpeg_4k` | mjpeg | 3840×2160 | 30 | — |
