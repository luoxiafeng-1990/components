# Components WebUI API 接口文档

> **版本**: v1.0  
> **基础路径**: `http://<host>:8080/api`  
> **数据格式**: JSON (`Content-Type: application/json`)

---

## 目录

1. [数据源管理 (DataSource)](#1-数据源管理-datasource)
2. [录制管理 (Recording)](#2-录制管理-recording)
3. [文件浏览 (FileSystem)](#3-文件浏览-filesystem)
4. [Worker 管理](#4-worker-管理)
5. [消费者管理 (Consumer)](#5-消费者管理-consumer)
6. [实时预览 (Preview)](#6-实时预览-preview)
7. [配置管理 (Config)](#7-配置管理-config)
8. [数据模型定义](#8-数据模型定义)

---

## 通用约定

### 响应格式

所有 API 返回统一的 JSON 格式：

```json
{
  "code": 0,
  "message": "success",
  "data": { ... }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `code` | int | 0=成功，非0=错误码 |
| `message` | string | 人类可读的描述 |
| `data` | object/array/null | 业务数据 |

### 错误码

| 错误码 | 说明 |
|--------|------|
| 0 | 成功 |
| 1001 | 参数错误 |
| 1002 | 资源不存在 |
| 1003 | 资源已存在（ID冲突） |
| 2001 | 数据源不可用（路径无效/RTSP不可达） |
| 2002 | 数据源正在使用中，不可删除 |
| 3001 | Worker 启动失败 |
| 3002 | Worker 状态不允许该操作 |
| 3003 | Worker 绑定的数据源不存在 |
| 4001 | 消费者类型不支持 |
| 4002 | 消费者配置无效 |
| 5001 | 预览服务不可用 |
| 5002 | 编码器初始化失败 |
| 9001 | 内部错误 |

---

## 1. 数据源管理 (DataSource)

### 1.1 获取数据源列表

```
GET /api/datasources
```

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": [
    {
      "id": "ds-001",
      "name": "摄像头1",
      "type": "RTSP",
      "path": "rtsp://192.168.1.100:554/stream1",
      "buffer_count": 5,
      "max_frames": -1,
      "loop": false,
      "created_at": "2026-04-03T10:00:00Z",
      "status": "idle"
    }
  ]
}
```

### 1.2 添加数据源

```
POST /api/datasources
```

**请求体**:

```json
{
  "name": "摄像头1",
  "type": "FILE",
  "path": "/data/videos/test.mp4",
  "buffer_count": 5,
  "max_frames": -1,
  "loop": true
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 数据源名称 |
| `type` | string | 是 | 数据源类型：`FILE` / `RTSP` / `BUFFER` |
| `path` | string | 是 | 数据源路径或URL |
| `buffer_count` | int | 否 | BufferPool 的 Buffer 数量，默认0（使用 Worker 默认值） |
| `max_frames` | int | 否 | 最大读取帧数，-1=无限制（默认） |
| `loop` | bool | 否 | 是否循环播放，默认 false |

**响应**:

```json
{
  "code": 0,
  "message": "数据源添加成功",
  "data": {
    "id": "ds-002",
    "name": "摄像头1",
    "type": "FILE",
    "path": "/data/videos/test.mp4",
    "buffer_count": 5,
    "max_frames": -1,
    "loop": true,
    "created_at": "2026-04-03T10:30:00Z",
    "status": "idle"
  }
}
```

### 1.3 修改数据源

```
PUT /api/datasources/{id}
```

**路径参数**:

| 参数 | 说明 |
|------|------|
| `id` | 数据源 ID |

**请求体**: 同 1.2，字段均为可选（仅更新提供的字段）。

> ⚠️ 如果数据源已被 Worker 绑定且 Worker 正在运行，修改将返回错误 `2002`。

### 1.4 删除数据源

```
DELETE /api/datasources/{id}
```

> ⚠️ 如果数据源被任何 Worker 绑定，返回错误 `2002`，需先解绑或删除对应 Worker。

**响应**:

```json
{
  "code": 0,
  "message": "数据源已删除",
  "data": null
}
```

### 1.5 探测数据源信息

```
GET /api/datasources/{id}/probe
```

对数据源执行探测（类似 `ffprobe`），返回媒体信息。

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "format": "h264",
    "codec": "h264",
    "width": 1920,
    "height": 1080,
    "fps": 30.0,
    "duration_seconds": 120.5,
    "bitrate": 4000000,
    "pixel_format": "yuv420p"
  }
}
```

> 对 RTSP 源，`duration_seconds` 为 -1（直播流无时长）。

### 1.6 预览数据源

```
GET /api/datasources/{id}/preview
```

根据数据源类型返回不同内容：

| 数据源类型 | 行为 | Content-Type |
|-----------|------|-------------|
| `FILE` (mp4/mkv等) | 返回视频文件流（支持 HTTP Range） | `video/mp4` 等 |
| `RTSP` | 返回 `.m3u` 播放列表文件，触发本地 VLC 打开 | `audio/x-mpegurl` + `Content-Disposition: attachment; filename="stream.m3u"` |
| `BUFFER` | 不支持预览，返回错误 | - |

**RTSP `.m3u` 文件内容示例**:

```
#EXTM3U
#EXTINF:-1,摄像头1
rtsp://192.168.1.100:554/stream1
```

> 💡 前端需提示用户安装 VLC 播放器。

---

## 2. 录制管理 (Recording)

### 2.1 开始录制

```
POST /api/datasources/{id}/record/start
```

**请求体**:

```json
{
  "format": "mp4",
  "output_dir": "/data/recordings/",
  "max_duration_seconds": 300,
  "max_frames": -1
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `format` | string | 否 | 录制格式：`mp4` / `h264` / `h265`，默认 `mp4` |
| `output_dir` | string | 否 | 输出目录，默认 `/data/recordings/` |
| `max_duration_seconds` | float | 否 | 最大录制时长（秒），-1=无限制 |
| `max_frames` | int | 否 | 最大录制帧数，-1=无限制 |

**响应**:

```json
{
  "code": 0,
  "message": "录制已开始",
  "data": {
    "recording_id": "rec-001",
    "datasource_id": "ds-001",
    "file_path": "/data/recordings/ds-001_20260403_103000.mp4",
    "started_at": "2026-04-03T10:30:00Z"
  }
}
```

### 2.2 停止录制

```
POST /api/datasources/{id}/record/stop
```

**响应**:

```json
{
  "code": 0,
  "message": "录制已停止",
  "data": {
    "recording_id": "rec-001",
    "file_path": "/data/recordings/ds-001_20260403_103000.mp4",
    "duration_seconds": 45.2,
    "file_size_bytes": 18432000,
    "total_frames": 1356
  }
}
```

### 2.3 获取录制文件列表

```
GET /api/recordings
```

**查询参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `datasource_id` | string | 可选，按数据源过滤 |

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": [
    {
      "id": "rec-001",
      "datasource_id": "ds-001",
      "file_path": "/data/recordings/ds-001_20260403_103000.mp4",
      "format": "mp4",
      "duration_seconds": 45.2,
      "file_size_bytes": 18432000,
      "created_at": "2026-04-03T10:30:00Z"
    }
  ]
}
```

### 2.4 删除录制文件

```
DELETE /api/recordings/{id}
```

同时删除磁盘上的录制文件。

### 2.5 播放录制文件

```
GET /api/recordings/{id}/play
```

返回视频文件流（支持 HTTP Range），可在浏览器 `<video>` 标签中播放。

---

## 3. 文件浏览 (FileSystem)

### 3.1 浏览目录

```
GET /api/filesystem/browse
```

**查询参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `path` | string | 目录路径，默认 `/` |
| `filter` | string | 可选，文件类型过滤：`video`（仅视频文件）/ `all`（默认） |

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "current_path": "/data/videos/",
    "parent_path": "/data/",
    "entries": [
      {
        "name": "test_1080p.mp4",
        "path": "/data/videos/test_1080p.mp4",
        "type": "file",
        "size_bytes": 52428800,
        "modified_at": "2026-04-01T08:00:00Z",
        "extension": ".mp4"
      },
      {
        "name": "samples",
        "path": "/data/videos/samples/",
        "type": "directory",
        "size_bytes": 0,
        "modified_at": "2026-03-28T12:00:00Z",
        "extension": ""
      }
    ]
  }
}
```

> `filter=video` 时仅返回 `.mp4`, `.mkv`, `.avi`, `.h264`, `.h265`, `.265`, `.264`, `.ts`, `.flv` 等视频文件和目录。

---

## 4. Worker 管理

### 4.1 获取 Worker 列表

```
GET /api/workers
```

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": [
    {
      "id": "wk-001",
      "name": "Worker-1",
      "datasource_id": "ds-001",
      "datasource_name": "摄像头1",
      "state": "RUNNING",
      "worker_type": "FFMPEG_DECODE",
      "decoder": {
        "name": null,
        "enable_hardware": true,
        "decode_threads": 0
      },
      "created_at": "2026-04-03T10:30:00Z",
      "consumers": ["DISPLAY", "JPEG_PREVIEW"]
    }
  ]
}
```

### 4.2 创建 Worker

```
POST /api/workers
```

**请求体**:

```json
{
  "name": "Worker-1",
  "datasource_id": "ds-001",
  "worker_type": "FFMPEG_DECODE",
  "decoder": {
    "name": null,
    "enable_hardware": true,
    "decode_threads": 0,
    "vendor": {
      "type": "taco",
      "config": {}
    }
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | Worker 名称 |
| `datasource_id` | string | 是 | 绑定的数据源 ID |
| `worker_type` | string | 否 | Worker 类型：`AUTO` / `FFMPEG_DECODE` / `FFMPEG_ENCODE` / `FFMPEG_PACKET_RECORDER`，默认 `FFMPEG_DECODE` |
| `decoder` | object | 否 | 解码器配置 |
| `decoder.name` | string\|null | 否 | 解码器名称，null=自动选择 |
| `decoder.enable_hardware` | bool | 否 | 启用硬件加速，默认 true |
| `decoder.decode_threads` | int | 否 | 解码线程数，0=自动 |
| `decoder.vendor` | object | 否 | 厂商扩展配置 |

**响应**: 返回创建的 Worker 完整信息（同 4.1 列表中的单条记录）。

### 4.3 删除 Worker

```
DELETE /api/workers/{id}
```

> ⚠️ 如果 Worker 正在运行，会先自动停止再删除。

### 4.4 启动 Worker

```
POST /api/workers/{id}/start
```

启动 Worker 后，Worker 开始解码并往 BufferPool 填充 Buffer。

**响应**:

```json
{
  "code": 0,
  "message": "Worker 已启动",
  "data": {
    "id": "wk-001",
    "state": "RUNNING"
  }
}
```

### 4.5 停止 Worker

```
POST /api/workers/{id}/stop
```

停止 Worker 及其所有关联的消费者。

### 4.6 获取 Worker 实时状态

```
GET /api/workers/{id}/status
```

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "id": "wk-001",
    "state": "RUNNING",
    "fps": 29.97,
    "decoded_frames": 1830,
    "dropped_frames": 2,
    "uptime_seconds": 61.2,
    "buffer_pool": {
      "total": 5,
      "free": 2,
      "filled": 3
    },
    "consumers": [
      {
        "id": "cs-001",
        "type": "DISPLAY",
        "state": "active",
        "processed_frames": 1828
      },
      {
        "id": "cs-002",
        "type": "JPEG_PREVIEW",
        "state": "active",
        "fps": 15,
        "processed_frames": 915
      }
    ]
  }
}
```

---

## 5. 消费者管理 (Consumer)

### 5.1 获取 Worker 的消费者列表

```
GET /api/workers/{id}/consumers
```

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": [
    {
      "id": "cs-001",
      "type": "DISPLAY",
      "state": "active",
      "config": {
        "device_id": 0
      }
    }
  ]
}
```

### 5.2 为 Worker 添加消费者

```
POST /api/workers/{id}/consumers
```

**请求体**（按类型不同）:

#### DISPLAY（HDMI 显示）

```json
{
  "type": "DISPLAY",
  "config": {
    "device_id": 0
  }
}
```

#### SAVE_RAW（保存原始数据）

```json
{
  "type": "SAVE_RAW",
  "config": {
    "output_paths": ["/data/output/ch0.yuv"]
  }
}
```

#### SAVE_ENCODED（保存编码流）

```json
{
  "type": "SAVE_ENCODED",
  "config": {
    "output_path": "/data/output/stream.h264",
    "format": "h264"
  }
}
```

#### COMPARE（解码质量分析 / 通道比较）

```json
{
  "type": "COMPARE",
  "config": {
    "reference_worker_id": "wk-002",
    "metrics": ["psnr", "ssim"]
  }
}
```

#### OPENCV（OpenCV 消费）

```json
{
  "type": "OPENCV",
  "config": {
    "operations": ["psnr", "ssim"]
  }
}
```

#### NPU_INFERENCE（NPU 推理）

```json
{
  "type": "NPU_INFERENCE",
  "config": {
    "model_path": "/models/yolov5.model",
    "threshold": 0.5,
    "nms_threshold": 0.45
  }
}
```

#### JPEG_PREVIEW（JPEG 编码预览）

```json
{
  "type": "JPEG_PREVIEW",
  "config": {
    "encoder_name": "jpeg_taco",
    "quality": 80,
    "target_fps": 15
  }
}
```

| 字段 | 说明 |
|------|------|
| `encoder_name` | 使用 components enc 模块的编码器：`jpeg_taco`（硬件）/ `mjpeg`（软件） |
| `quality` | JPEG 质量 1-100 |
| `target_fps` | 预览帧率（从 BufferPool 取帧的频率，降低浏览器带宽） |

> 💡 JPEG_PREVIEW 本质上是通过 MultiWorker 模式创建一个 `FFmpegEncodeWorker`（`WorkerType::FFMPEG_ENCODE`），配置编码器为 `jpeg_taco` 或 `mjpeg`，以 `buffer_mode=true` 从解码 Worker 的 BufferPool 消费。这完全使用 components 自身的 enc 模块能力。

**消费者类型汇总**:

| 类型常量 | 对应 ConsumeTypeFlags | 说明 |
|----------|----------------------|------|
| `DISPLAY` | `CONSUME_DISPLAY` | HDMI/Framebuffer 显示 |
| `SAVE_RAW` | `CONSUME_SAVE_RAW` | 保存解码后原始 YUV/RGB |
| `SAVE_ENCODED` | `CONSUME_SAVE_ENCODED` | 保存编码流 |
| `COMPARE` | `CONSUME_CHANNEL_COMPARE` | 通道比较（PSNR/SSIM） |
| `OPENCV` | `CONSUME_OPENCV` | OpenCV 消费 |
| `NPU_INFERENCE` | `CONSUME_NPU_INFERENCE` | NPU 推理 |
| `JPEG_PREVIEW` | — (enc 模块消费者 Worker) | JPEG 编码预览（使用 FFmpegEncodeWorker） |
| `COUNT` | `CONSUME_COUNT` | 仅统计帧数 |

### 5.3 移除消费者

```
DELETE /api/workers/{id}/consumers/{consumer_id}
```

### 5.4 修改消费者配置

```
PUT /api/workers/{id}/consumers/{consumer_id}
```

请求体同 5.2，仅更新 `config` 中提供的字段。

---

## 6. 实时预览 (Preview)

### 6.1 获取单路 MJPEG 视频流

```
GET /api/preview/stream/{worker_id}
```

**响应**: 持续的 MJPEG 流

```
Content-Type: multipart/x-mixed-replace; boundary=frame

--frame
Content-Type: image/jpeg
Content-Length: 23456

<JPEG binary data>
--frame
Content-Type: image/jpeg
Content-Length: 23123

<JPEG binary data>
...
```

> 前端使用方式：`<img src="/api/preview/stream/wk-001" />`
>
> ⚠️ 前提：该 Worker 必须已添加 `JPEG_PREVIEW` 消费者且 Worker 正在运行。

### 6.2 获取单帧截图

```
GET /api/preview/snapshot/{worker_id}
```

**查询参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `quality` | int | JPEG 质量 1-100，默认 80 |

**响应**: 单张 JPEG 图片

```
Content-Type: image/jpeg
```

### 6.3 获取多路预览布局信息

```
GET /api/preview/grid
```

**查询参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `layout` | string | 布局：`2x2` / `3x3` / `4x4`，默认 `3x3` |

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "layout": "3x3",
    "total_slots": 9,
    "streams": [
      {
        "slot": 0,
        "worker_id": "wk-001",
        "worker_name": "Worker-1",
        "stream_url": "/api/preview/stream/wk-001",
        "state": "RUNNING"
      },
      {
        "slot": 1,
        "worker_id": "wk-002",
        "worker_name": "Worker-2",
        "stream_url": "/api/preview/stream/wk-002",
        "state": "RUNNING"
      }
    ]
  }
}
```

> 前端根据 `streams` 数组，将每个 `stream_url` 放入对应格子的 `<img>` 标签中。

---

## 7. 配置管理 (Config)

### 7.1 导出配置

```
GET /api/config/export
```

导出当前全部配置（数据源 + Worker + 消费者）为 JSON。

**响应**:

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "version": "1.0",
    "exported_at": "2026-04-03T10:30:00Z",
    "datasources": [ ... ],
    "workers": [ ... ]
  }
}
```

### 7.2 导入配置

```
POST /api/config/import
```

**请求体**: 同 7.1 导出的 `data` 字段内容。

**查询参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `mode` | string | `replace`（覆盖，默认）/ `merge`（合并） |

---

## 8. 数据模型定义

### 8.1 DataSource

```json
{
  "id": "string",
  "name": "string",
  "type": "FILE | RTSP | BUFFER",
  "path": "string",
  "buffer_count": "int (default: 0)",
  "max_frames": "int (default: -1)",
  "loop": "bool (default: false)",
  "created_at": "ISO8601 datetime",
  "status": "idle | recording | in_use"
}
```

**与 C++ 结构体的映射关系**:

| API 字段 | C++ 结构体字段 | 说明 |
|----------|---------------|------|
| `path` | `WorkerConfig::DataSourceConfig::path` | 数据源路径/URL |
| `buffer_count` | `WorkerConfig::DataSourceConfig::buffer_count` | Buffer 数量 |
| `max_frames` | `WorkerConfig::DataSourceConfig::max_frames` | 最大帧数 |
| `loop` | `WorkerConfig::DataSourceConfig::loop` | 循环播放 |
| `type` | 由 `IDataSourceNavigator::SourceType` 推断 | FILE/RTSP/BUFFER |

> 注意：`DataSourceConfig` 中的 `buffer_mode`, `codec_params`, `time_base`, `shared_packet_source`, `deferred_commit` 为**内部参数**，由框架自动设置，API 不暴露。

### 8.2 Worker

```json
{
  "id": "string",
  "name": "string",
  "datasource_id": "string",
  "state": "CREATED | STARTING | RUNNING | STOPPING | STOPPED | ERROR",
  "worker_type": "AUTO | FFMPEG_DECODE | FFMPEG_ENCODE | FFMPEG_PACKET_RECORDER",
  "decoder": {
    "name": "string | null",
    "enable_hardware": "bool",
    "decode_threads": "int"
  },
  "created_at": "ISO8601 datetime",
  "consumers": ["string (consumer type list)"]
}
```

**与 C++ 结构体的映射关系**:

| API 字段 | C++ 结构体字段 |
|----------|---------------|
| `worker_type` | `WorkerConfig::GlobalConfig::worker_type` |
| `decoder.name` | `WorkerConfig::DecoderConfig::name` |
| `decoder.enable_hardware` | `WorkerConfig::DecoderConfig::enable_hardware` |
| `decoder.decode_threads` | `WorkerConfig::DecoderConfig::decode_threads` |

### 8.3 Consumer

```json
{
  "id": "string",
  "type": "DISPLAY | SAVE_RAW | SAVE_ENCODED | COMPARE | OPENCV | NPU_INFERENCE | JPEG_PREVIEW | COUNT",
  "state": "active | inactive | error",
  "config": { "... (按类型不同)" }
}
```

**与 C++ 的映射关系**:

| API 消费者类型 | C++ 实现 |
|---------------|---------|
| `DISPLAY` | `DisplayConsumer` + `ConsumeTypeFlags::CONSUME_DISPLAY` |
| `SAVE_RAW` | `SaveRawConsumer` + `CONSUME_SAVE_RAW` |
| `SAVE_ENCODED` | `SaveEncodedConsumer` + `CONSUME_SAVE_ENCODED` |
| `COMPARE` | `ChannelCompareConsumer` + `CONSUME_CHANNEL_COMPARE` |
| `OPENCV` | `OpencvConsumer` + `CONSUME_OPENCV` |
| `NPU_INFERENCE` | `NpuInferenceConsumer` + `CONSUME_NPU_INFERENCE` |
| `JPEG_PREVIEW` | `FFmpegEncodeWorker`（MultiWorker Consumer Worker 模式，encoder=jpeg_taco/mjpeg） |
| `COUNT` | `CountConsumer` + `CONSUME_COUNT` |

### 8.4 Recording

```json
{
  "id": "string",
  "datasource_id": "string",
  "file_path": "string",
  "format": "mp4 | h264 | h265",
  "duration_seconds": "float",
  "file_size_bytes": "int",
  "total_frames": "int",
  "created_at": "ISO8601 datetime"
}
```

### 8.5 FileEntry

```json
{
  "name": "string",
  "path": "string",
  "type": "file | directory",
  "size_bytes": "int",
  "modified_at": "ISO8601 datetime",
  "extension": "string"
}
```

---

## 附录 A：预览编码 Pipeline 说明

JPEG 预览使用 components 自身的 enc 模块（`FFmpegEncodeWorker`），通过 MultiWorker Producer-Consumer 模式实现：

```
┌──────────────────────┐      Connector       ┌──────────────────────┐
│  Producer:           │     ONE_TO_MANY       │  Consumer Worker:    │
│  FFmpegDecodeWorker  │ ───────────────────→  │  FFmpegEncodeWorker  │
│  (解码 raw frames)   │   buffer_mode=true    │  encoder=jpeg_taco   │
│                      │                       │  quality=80          │
└──────────────────────┘                       └──────────┬───────────┘
                                                          │
                                                    JPEG frames
                                                          │
                                                          ▼
                                               API PreviewService
                                               → MJPEG HTTP Stream
                                               → 浏览器 <img> 显示
```

**关键配置映射**:

```cpp
// 解码 Worker (Producer)
WorkerConfig decode_config;
decode_config.data_source.path = datasource.path;
decode_config.global.worker_type = WorkerType::FFMPEG_DECODE;

// JPEG 编码 Worker (Consumer Worker)
WorkerConfig encode_config;
encode_config.data_source.buffer_mode = true;  // 框架自动设置
encode_config.global.worker_type = WorkerType::FFMPEG_ENCODE;
encode_config.encoder.name = "jpeg_taco";      // 或 "mjpeg"
encode_config.encoder.jpeg.quality = 80;

// Connector
ConnectorConfig connector;
connector.mode = Connector::Mode::ONE_TO_MANY;
connector.producer_names = {"decode-worker"};
connector.consumer_names = {"jpeg-encode-worker", "other-consumers..."};
```

---

## 附录 B：RTSP 预览说明

RTSP 数据源的预览通过触发用户本地的 VLC 播放器实现：

1. 用户点击 RTSP 数据源的"预览"按钮
2. 前端请求 `GET /api/datasources/{id}/preview`
3. 后端返回 `.m3u` 播放列表文件
4. 浏览器下载 `.m3u` 文件，系统关联的 VLC 自动打开并播放 RTSP 流

**前提条件**：用户本机需安装 VLC 播放器并关联 `.m3u` 文件类型。

---

## 附录 C：WebSocket 事件（规划中）

后续版本可增加 WebSocket 通道用于实时事件推送：

```
WS /api/ws
```

**事件类型**:

| 事件 | 说明 |
|------|------|
| `worker.state_changed` | Worker 状态变更 |
| `worker.stats_update` | Worker 统计信息更新（每秒） |
| `consumer.error` | 消费者错误 |
| `recording.completed` | 录制完成 |
| `system.alert` | 系统告警 |
