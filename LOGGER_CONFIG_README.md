# 日志配置使用说明

## 🎯 概述

本项目使用 `log4cplus` 实现模块化、层次化日志系统。支持**配置文件**和**编程式**两种配置方式。

**推荐使用配置文件方式**，因为：
- ✅ 无需重新编译
- ✅ 可以快速调整日志级别进行调试
- ✅ 支持多种调试场景快速切换

---

## 📂 配置文件位置

程序会按以下顺序查找配置文件（找到第一个就停止）：

1. `./logger.properties`（当前目录，**推荐**）
2. `/etc/logger.properties`（系统配置）
3. `../logger.properties`（上级目录）

---

## 🚀 快速开始

### 方法 1：使用完整配置文件（推荐）

```bash
# 1. 复制模板
cp logger.properties ./logger.properties

# 2. 根据需要修改配置文件
vim logger.properties

# 3. 直接运行程序（无需重新编译）
./display_test -m multi_worker video.mp4
```

### 方法 2：使用最小化配置文件

```bash
# 1. 复制最小化模板
cp logger.properties.minimal ./logger.properties

# 2. 直接运行程序
./display_test -m multi_worker video.mp4
```

### 方法 3：不使用配置文件（硬编码方式）

```bash
# 1. 删除或重命名配置文件
rm -f ./logger.properties

# 2. 运行程序（使用 test.cpp 中的硬编码配置）
./display_test -m multi_worker video.mp4

# 注意：修改日志级别需要编辑 test.cpp 并重新编译
```

---

## 📝 日志级别说明

从详细到简略（从低到高）：

| 级别    | 说明                         | 适用场景                     |
|---------|------------------------------|------------------------------|
| `TRACE` | 最详细的跟踪信息             | 深度调试（会产生大量日志）   |
| `DEBUG` | 详细的调试信息               | 开发调试                     |
| `INFO`  | 重要的业务流程信息           | 正常运行（默认）             |
| `WARN`  | 警告信息（不影响正常运行）   | 生产环境                     |
| `ERROR` | 错误信息（可能影响功能）     | 生产环境                     |
| `FATAL` | 致命错误（导致程序崩溃）     | 生产环境                     |

---

## 🔧 常见调试场景配置

### 场景 1：调试 RTSP 连接问题

```properties
# 在 logger.properties 中修改
log4cplus.logger.components.DataSource.Rtsp=TRACE
log4cplus.logger.components.Worker.Rtsp=TRACE
```

### 场景 2：调试内存分配问题

```properties
log4cplus.logger.components.Allocator=DEBUG
log4cplus.logger.components.Allocator.AVFrame=TRACE
log4cplus.logger.components.BufferPool=DEBUG
```

### 场景 3：调试多 Worker 调度问题

```properties
log4cplus.logger.components.MultiWorker=TRACE
log4cplus.logger.components.Connector=DEBUG
```

### 场景 4：只看关键信息（生产环境）

```properties
log4cplus.rootLogger=WARN, CONSOLE
log4cplus.logger.components=WARN
```

### 场景 5：完全静默（只输出错误）

```properties
log4cplus.rootLogger=ERROR, CONSOLE
log4cplus.logger.components=ERROR
```

---

## 🌲 层次化 Logger 说明

### 什么是层次化？

```
components                     (父模块)
├── components.Worker          (子模块 - 会继承父模块设置)
│   ├── components.Worker.VideoFile
│   ├── components.Worker.Rtsp
│   └── components.Worker.Recorder
└── components.Allocator       (子模块)
    ├── components.Allocator.AVFrame
    ├── components.Allocator.Framebuffer
    └── components.Allocator.Normal
```

### 继承规则

- 父模块的设置会**自动继承**给所有子模块
- 子模块可以**覆盖**父模块的设置

### 示例

```properties
# 父模块设置 DEBUG（所有 Worker.* 子模块都会是 DEBUG）
log4cplus.logger.components.Worker=DEBUG

# 单独设置某个子模块为 INFO（覆盖父模块）
log4cplus.logger.components.Worker.Recorder=INFO
```

**结果**：
- `components.Worker.VideoFile` → `DEBUG`（继承）
- `components.Worker.Rtsp` → `DEBUG`（继承）
- `components.Worker.Recorder` → `INFO`（覆盖）

---

## 📊 所有可配置模块列表

| 模块名称                                  | 说明                     | 推荐级别 |
|-------------------------------------------|--------------------------|----------|
| `components.Test`                         | 测试框架                 | INFO     |
| `components.MultiWorker`                  | 多 Worker 生产线         | DEBUG    |
| `components.VideoLine`                    | 视频生产线               | INFO     |
| `components.BufferPool`                   | 缓冲池管理               | WARN     |
| `components.BufferPacketSource`           | 数据包源（共享）         | INFO     |
| `components.BufferWriter`                 | 缓冲写入器               | INFO     |
| `components.BufferComparator`             | 缓冲比较器               | INFO     |
| `components.BufferPool.Registry`          | 缓冲池注册表             | WARN     |
| `components.Connector`                    | Worker 连接器            | INFO     |
| `components.Worker`                       | Worker 基类（父模块）    | DEBUG    |
| `components.Worker.VideoFile`             | 文件视频解码 Worker      | DEBUG    |
| `components.Worker.Rtsp`                  | RTSP 解码 Worker         | DEBUG    |
| `components.Worker.Recorder`              | 录制 Worker              | INFO     |
| `components.Worker.Factory`               | Worker 工厂              | WARN     |
| `components.Worker.Facade`                | Worker 外观              | WARN     |
| `components.Allocator`                    | 分配器基类（父模块）     | WARN     |
| `components.Allocator.AVFrame`            | AVFrame 分配器           | WARN     |
| `components.Allocator.Framebuffer`        | Framebuffer 分配器       | WARN     |
| `components.Allocator.Normal`             | 普通内存分配器           | WARN     |
| `components.Allocator.Factory`            | 分配器工厂               | WARN     |
| `components.Allocator.Facade`             | 分配器外观               | WARN     |
| `components.DataSource.Rtsp`              | RTSP 数据源              | INFO     |
| `components.DataSource.File`              | 文件数据源               | INFO     |
| `components.Display.Framebuffer`          | Framebuffer 显示设备     | INFO     |
| `components.Monitor.Performance`          | 性能监控器               | INFO     |
| `components.Util.Timer`                   | 计时器工具               | INFO     |

---

## 🎨 日志输出格式

```
[2026-01-15 18:04:28.960] [components.Worker.Recorder] [INFO ] 📡 Opening data source
│                          │                            │        │
│                          │                            │        └─ 日志消息
│                          │                            └─ 日志级别（固定5字符对齐）
│                          └─ Logger 名称（动态宽度，显示完整层次路径）
└─ 时间戳（精确到毫秒）
```

---

## ❓ 常见问题

### Q1: 修改配置文件后需要重新编译吗？

**A:** 不需要！直接运行程序即可生效。

### Q2: 配置文件和硬编码配置哪个优先级高？

**A:** 配置文件优先级更高。如果找到配置文件，硬编码配置会被跳过。

### Q3: 如何临时启用某个模块的详细日志？

**A:** 在 `logger.properties` 中修改对应模块的级别为 `DEBUG` 或 `TRACE`，然后重新运行程序。

### Q4: 如何确认程序使用了哪个配置文件？

**A:** 程序启动时会输出：
```
✅ 日志配置文件已加载: ./logger.properties
```

或者如果没有配置文件：
```
⚠️  未找到日志配置文件，使用默认配置（所有模块 DEBUG 级别）
💡 提示：创建 ./logger.properties 可自定义日志级别（无需重新编译）
```

### Q5: 如何完全禁用某个模块的日志？

**A:** 将该模块的级别设置为 `OFF`：
```properties
log4cplus.logger.components.BufferPool=OFF
```

---

## 📦 文件清单

```
packages/components/
├── logger.properties                  # 完整配置文件模板（推荐复制到当前目录）
├── logger.properties.minimal          # 最小化配置文件模板
├── LOGGER_CONFIG_README.md            # 本说明文档
├── include/common/Logger.hpp          # 日志系统实现
└── test_cases/dec/test.cpp            # 测试程序（包含硬编码配置）
```

---

## 🔗 相关链接

- [log4cplus 官方文档](https://github.com/log4cplus/log4cplus)
- [log4cplus 配置文件格式](https://log4cplus.sourceforge.io/docs/html/configuration.html)

---

**🎉 祝调试顺利！**
