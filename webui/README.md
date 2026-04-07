# Components WebUI

基于 Web 浏览器的 Components 组件管理界面，通过 RESTful API 与 C++ 组件层通信。

## 架构

```
Browser (Vue 3 + Element Plus)
    │
    │ REST API (JSON) / MJPEG Stream
    ▼
C++ API Server (cpp-httplib)
    │
    │ 直接调用
    ▼
Components Library (WorkerConfig / BufferPool / Consumer / ...)
```

## 功能

- **数据源管理**: 添加/删除/编辑数据源（FILE/RTSP/BUFFER），支持文件浏览器选择、RTSP VLC 预览、MP4 在线播放
- **Worker 管理**: 创建/启动/停止 Worker，绑定数据源，实时查看解码统计
- **消费者配置**: 为 Worker 叠加消费类型（HDMI 显示、保存、质量分析、NPU 推理、JPEG 预览等）
- **实时预览**: 通过 components enc 模块编码 JPEG，MJPEG 流在浏览器显示，支持 1/4/9/16 宫格
- **配置持久化**: JSON 文件存储，支持导入/导出

## 快速开始

### 前提条件

- C++17 编译器 (g++ 8+)
- Node.js 18+ 和 npm
- VLC 播放器（可选，用于 RTSP 预览）

### 编译

```bash
cd components/webui

# 编译后端
make backend

# 构建前端
make frontend

# 或者一起
make all
```

### 运行

```bash
# 默认端口 8080
make run

# 自定义参数
./build/webui_server --port 9090 --static dist --config /path/to/config.json
```

### 开发模式

前端热重载开发：

```bash
# 终端 1: 启动后端
./build/webui_server --port 8080

# 终端 2: 启动前端开发服务器（自动代理 API 到 8080）
cd frontend
npm install
npm run dev
# 访问 http://localhost:3000
```

## 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--port <port>` | HTTP 端口 | 8080 |
| `--host <host>` | 绑定地址 | 0.0.0.0 |
| `--config <path>` | 配置文件路径 | `~/.components/webui_config.json` |
| `--static <dir>` | 前端静态文件目录 | (无) |
| `--recordings <dir>` | 录制输出目录 | `/data/recordings` |

## API 文档

完整 API 接口文档见 [docs/API.md](docs/API.md)。

## 目录结构

```
webui/
├── docs/
│   └── API.md                  # API 接口文档（对外开放）
├── backend/
│   ├── include/                # C++ 头文件
│   │   ├── ApiTypes.hpp        # 数据模型与序列化
│   │   ├── WebServer.hpp       # HTTP 服务主类
│   │   ├── DataSourceManager.hpp
│   │   ├── WorkerManager.hpp
│   │   ├── ConsumerManager.hpp
│   │   ├── PreviewService.hpp
│   │   └── ConfigStore.hpp
│   ├── source/                 # C++ 实现
│   │   ├── main.cpp            # 入口
│   │   ├── WebServer.cpp       # 路由注册与请求处理
│   │   ├── DataSourceManager.cpp
│   │   ├── WorkerManager.cpp
│   │   ├── ConsumerManager.cpp
│   │   ├── PreviewService.cpp
│   │   └── ConfigStore.cpp
│   └── third_party/            # 第三方依赖（header-only）
│       ├── httplib.h           # cpp-httplib
│       └── nlohmann/json.hpp   # nlohmann/json
├── frontend/
│   ├── src/
│   │   ├── api/                # API 调用封装
│   │   ├── stores/             # Pinia 状态管理
│   │   ├── views/              # 页面组件
│   │   │   ├── DataSourceView.vue
│   │   │   ├── WorkerView.vue
│   │   │   └── PreviewView.vue
│   │   ├── components/         # 可复用组件
│   │   ├── router/             # 路由配置
│   │   ├── App.vue
│   │   └── main.ts
│   ├── package.json
│   └── vite.config.ts
├── Makefile
└── README.md
```

## 与 Components 的集成

WebUI 后端通过直接链接 `libcomponents` 库来调用组件功能。当前实现包含占位逻辑（模拟解码循环），后续需要对接：

1. `FFmpegDecodeWorker` — Worker 解码循环
2. `FFmpegEncodeWorker` — JPEG 预览编码（enc 模块）
3. `MultiWorkerProductionLine` — Producer-Consumer Worker 协作
4. `BufferConsumerService` — 消费者管理
5. `IEncodedPacketSource` — 数据源探测与打开

### 预览编码 Pipeline

```
DecodeWorker → BufferPool → EncodeWorker(jpeg_taco/mjpeg) → MJPEG Stream → Browser
```

预览使用 MultiWorker 模式：Connector(ONE_TO_MANY) 将解码 Worker 的 BufferPool
共享给 JPEG 编码 Worker（`buffer_mode=true`），编码 Worker 使用 components 的
`FFmpegEncodeWorker` 配置为 JPEG 编码器。
