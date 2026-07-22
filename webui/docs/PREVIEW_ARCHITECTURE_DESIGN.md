# WebUI 预览架构重构设计

状态：已认可  
方案：方案 3（完整解耦）  
日期：2026-07-19

## 1. 背景

当前 WebUI 只显示一条 Composite MJPEG 流，但页面使用静态
`JPEG_PREVIEW` Consumer 判断哪些 Worker 可预览。与此同时，
PARALLEL 模式将所有 Worker 的消费 flags 合并后再传给每个 Worker，
导致任意一路启用 JPEG 后，其他 Worker 也可能创建 JPEG 编码器。

产品定义中的“单路预览”不是静态 Consumer，而是用户交互：

1. 用户在 Composite 宫格中双击某个通道；
2. WebUI 向后端发送显式 START 消息；
3. 后端仅为指定 Worker 创建单路 JPEG 编码会话；
4. 页面显示该 Worker 的放大画面；
5. 用户再次双击，WebUI 发送显式 STOP 消息；
6. 后端释放对应编码资源，页面返回 Composite。

Composite 路径目前还存在三次原始帧复制：

1. Stitcher DMA Buffer 复制到 `composite_raw_buf_`；
2. `composite_raw_buf_` 复制到编码线程的 `nv12_copy`；
3. `jpeg_taco` 因 AVFrame 缺少 DMA metadata，再复制到自有 dmabuf。

Stitcher 当前虽然使用固定周期调用 `onTick()`，但 Render 线程仍会等待
所有活跃通道完成或超时。Render 未及时提交 FILLED Buffer 时，
`onTick()` 会直接跳过本次发送，与“固定节奏使用各通道最新帧输出”的
产品目标不一致。

## 2. 目标

本设计必须实现以下目标：

1. 单路预览由 WebUI 双击事件显式 START/STOP，不由静态
   `JPEG_PREVIEW` Consumer 驱动。
2. 未 START 的 Worker 不创建 JPEG 编码器、不复制预览帧。
3. 允许任意多个 Worker 被显式 START；实际并发能力由硬件资源决定。
4. 任意一路单路预览失败，不影响 Composite、IDS 或其他 Worker。
5. PARALLEL 模式下每个 Worker 只创建自己配置的 Consumer。
6. Composite 优先走 Stitcher DMA Buffer 到 `jpeg_taco` 的零拷贝路径。
7. 若目标硬件不接受 Stitcher Buffer block ID，则自动使用一次 DMA
   staging copy 的安全回退路径，禁止退回三次 CPU 拷贝。
8. Stitcher 按固定显示节奏发布当前最新 Composite；任一通道未更新时
   沿用该通道上一帧，不等待该通道。
9. 页面设置必须区分 IDS FPS、预览目标 FPS、编码 FPS 和 HTTP 发送 FPS。
10. 页面展示的实际指标必须来自真实流水线阶段，不能再以单路 JPEG
    回调 FPS 代替 Composite FPS。

## 3. 非目标

本次重构不包含：

1. 修改视频解码器实现；
2. 修改 NPU、保存、OpenCV 等无关 Consumer 的业务逻辑；
3. 改变 IDS 的分辨率、布局算法或驱动 ABI；
4. 引入 Kafka、RabbitMQ 等外部消息中间件；
5. 删除 QA/CLI 使用的 `JpegEncodeConsumer` 或 PreviewPlugin；
6. 将 MJPEG 协议替换为 WebRTC、HLS 或 WebSocket 视频协议。

## 4. 术语

### 4.1 Composite 预览

FrameStitcherService 将多个通道的原始帧拼成一张完整 NV12 画面，
然后该画面编码为一路 MJPEG，供 WebUI 宫格显示。

### 4.2 单路预览

用户双击 Composite 中的某个格子后，临时为对应 Worker 启动的独立
JPEG 预览会话。再次双击后停止。

### 4.3 FrameTap

WebUI Worker 原始帧路径中的轻量观察点。没有活跃单路会话时只执行
一次会话状态查询；有会话时才引用并转发该 Worker 的最新帧。

### 4.4 Frame Lease

带生命周期的 Buffer 引用。只有 IDS、Stitcher 和 Preview 编码线程都
释放引用后，底层 Buffer 才归还 BufferPool。

## 5. 总体架构

```text
                              控制平面

WebUI 双击
   │
   ├─ POST /api/preview/sessions
   └─ DELETE /api/preview/sessions/{session_id}
                         │
                         ▼
               PreviewSessionManager
             worker_id → EncoderInstance


                              数据平面

DataSource → Decode Worker → BufferPool → Consumer Chain
                                      │
                                      ├─ DISPLAY → FrameStitcher → IDS
                                      │                         └→ Composite JPEG
                                      │
                                      └─ FrameTap
                                           │
                                  该 Worker 有活跃会话？
                                      ├─ 否：立即返回
                                      └─ 是：latest-only queue
                                                  │
                                                  ▼
                                           单路 jpeg_taco
                                                  │
                                                  ▼
                                   /api/preview/stream/{worker_id}
```

控制平面和数据平面必须解耦。REST 请求只管理会话，不直接执行编码；
帧数据只通过 FrameTap 和 latest-only 队列进入编码线程。

## 6. 单路预览控制协议

### 6.1 创建会话

请求：

```http
POST /api/preview/sessions
Content-Type: application/json

{
  "worker_id": "wk-001",
  "fps": 15,
  "quality": 80,
  "encoder": "jpeg_taco"
}
```

成功响应：

```json
{
  "code": 0,
  "data": {
    "session_id": "preview-000001",
    "worker_id": "wk-001",
    "state": "RUNNING",
    "stream_url": "/api/preview/stream/wk-001?session_id=preview-000001",
    "fps": 15,
    "quality": 80,
    "encoder": "jpeg_taco"
  }
}
```

创建规则：

1. Worker 必须存在且处于 RUNNING；
2. `fps` 范围为 1～60；
3. `quality` 范围为 1～100；
4. 默认编码器为 `jpeg_taco`；
5. 第一个会话创建该 Worker 的 EncoderInstance；
6. 同一 Worker、相同配置的后续会话共享 EncoderInstance；
7. 同一 Worker 已有会话但新请求配置不同，返回 HTTP 409，避免一个
   页面静默改变其他页面的编码参数；
8. 软件层不限制不同 Worker 的会话数；
9. 硬件实例耗尽时返回 `ENCODER_RESOURCE_EXHAUSTED`，不得停止或重启
   已运行的会话。

### 6.2 停止会话

```http
DELETE /api/preview/sessions/preview-000001
```

停止规则：

1. 删除 session 与 Worker EncoderInstance 的关联；
2. 同一 Worker 仍有其他 session 时继续编码；
3. 最后一个 session 删除后销毁 EncoderInstance；
4. 对已删除 session 重复 DELETE 仍返回成功，保证幂等；
5. Worker 停止、删除或 WebServer 退出时强制清理相关 session。

### 6.3 查询状态

```http
GET /api/preview/sessions
GET /api/preview/sessions/{session_id}
```

状态至少包含：

- session_id；
- worker_id；
- state；
- encoder；
- target_fps；
- source_fps；
- encoded_fps；
- sent_fps；
- dropped_input_frames；
- encoded_frames；
- last_error。

## 7. PreviewSessionManager

PreviewSessionManager 是单路预览唯一的生命周期管理者。

核心状态：

```text
sessions_by_id:
  session_id → Session

encoders_by_worker:
  worker_id → EncoderInstance
```

EncoderInstance 包含：

- Worker ID；
- JPEG 配置；
- 编码输入 latest-only 队列；
- 编码线程；
- 输出 FrameBuffer；
- 引用该编码器的 session ID 集合；
- 原子状态；
- 阶段指标；
- 最近错误。

### 7.1 状态机

```text
STOPPED
   │ START
   ▼
STARTING
   ├─ 初始化成功 → RUNNING
   └─ 初始化失败 → ERROR → STOPPED

RUNNING
   │ 最后一个 STOP / Worker stop / Server shutdown
   ▼
STOPPING
   ▼
STOPPED
```

### 7.2 停止顺序

停止必须严格执行：

1. 将状态改为 STOPPING；
2. FrameTap 停止接收该 Worker 的新帧；
3. 唤醒编码线程；
4. 释放 latest-only 队列中的 AVFrame 引用；
5. 停止编码 Pipeline；
6. join 编码线程；
7. 释放临时 BufferPool 和编码器；
8. 删除 EncoderInstance；
9. 状态变为 STOPPED。

禁止先销毁编码器再停止 FrameTap，否则可能发生悬垂引用。

## 8. FrameTap 与单路帧传递

### 8.1 插入位置

FrameTap 只用于 WebUI 构建的 WorkerConfig，不作为用户可见 Consumer，
也不改变 QA/CLI 的默认行为。

FrameTap 位于解码后的 Consumer 路径中，在 Buffer 释放回原始 BufferPool
之前获得当前 AVFrame。

### 8.2 无会话快路径

```text
if (!PreviewSessionManager::hasActiveSession(worker_id)) {
    return;
}
```

该路径禁止：

- 分配 AVFrame；
- memcpy；
- 获取编码 BufferPool；
- 创建线程；
- 调用 JPEG 编码器。

### 8.3 有会话路径

1. 使用 `av_frame_ref()` 保留原始 AVFrame；
2. 保留 `pool_blk_id`、`pool_blk_vaddr` 等 metadata；
3. 将引用放入对应 Worker 的深度为 1 的队列；
4. 队列已有旧帧时释放旧帧并替换为新帧；
5. 不允许等待编码线程；
6. 编码完成后释放 AVFrame 引用。

此设计保证单路 `jpeg_taco` 可以继续进入现有 zero-copy 分支。

### 8.4 JPEG 输出广播

现有 `frame_queue` 是破坏性消费：一个 HTTP 客户端取走并清空队列后，
其他客户端可能拿不到同一帧。新实现不得让多个 session 竞争同一个队列。

每个 EncoderInstance 保存：

- `shared_ptr<const vector<uint8_t>> latest_jpeg`；
- 单调递增的 `jpeg_sequence`；
- condition variable。

每个 HTTP 连接独立保存 `last_sent_sequence`：

```text
编码完成
  → 原子替换 latest_jpeg
  → jpeg_sequence++
  → notify_all

客户端 A 根据自己的 last_sent_sequence 读取
客户端 B 根据自己的 last_sent_sequence 读取
```

任何客户端都不能移动或清空共享 JPEG。慢客户端只跳过旧 sequence，不阻塞
编码线程，也不影响其他客户端。

## 9. 修复 PARALLEL 全局 flags

当前实现将所有 Worker flags 做 OR，再把同一个 flags 传给每个 Worker。
新实现必须在每个并行任务内部根据该 WorkerConfig 独立生成 flags。

目标语义：

```text
Worker 1: DISPLAY
Worker 2: DISPLAY + NPU
Worker 3: SAVE_RAW

实际创建：
Worker 1 → DISPLAY
Worker 2 → DISPLAY + NPU
Worker 3 → SAVE_RAW
```

不得再出现某个 Worker 启用 JPEG、NPU 或 SAVE 后扩散到其他 Worker。

WebUI 的按需单路编码不通过这些静态 flags 创建。

## 10. Composite Frame Lease

### 10.1 类型

新增等价于以下语义的对象：

```cpp
struct StitchedFrameLease {
    std::shared_ptr<Buffer> buffer;
    uint64_t generation;
    int width;
    int height;
    int format;
    size_t data_size;
};
```

`buffer` 使用自定义释放器：

```text
最后一个 shared_ptr 被释放
  → BufferPool::releaseFilled(buffer)
```

StitchedFrameLease 必须持有 BufferPool 的有效引用，避免 Pool 已析构时执行
释放回调。

### 10.2 引用持有者

同一张 Composite Buffer 可以同时由以下对象持有：

- 当前 IDS Display；
- PreviewService 的 Composite 编码队列；
- FrameStitcherService 当前帧；
- 调试或指标采样逻辑。

任一持有者不得手动提前 release Buffer。

### 10.3 latest-only

PreviewService 的 Composite 编码输入队列深度为 1：

1. 新 Lease 到达时替换未编码的旧 Lease；
2. 被替换 Lease 自动释放；
3. 编码线程慢时只丢旧帧，不积压 Buffer；
4. BufferPool 不应因 WebUI 慢客户端而耗尽。

## 11. Composite 零拷贝

### 11.1 首选路径

PreviewService 从 StitchedFrameLease 取得：

- Buffer 虚拟地址；
- Buffer block ID；
- 宽高和 NV12 linesize；
- generation。

构造 AVFrame 时：

1. `data[0]` 指向 DMA Buffer Y 平面；
2. `data[1]` 指向 DMA Buffer UV 平面；
3. metadata 写入 `pool_blk_id`；
4. metadata 写入 `pool_blk_vaddr`；
5. Lease 持有到 `avcodec_send_frame` 和对应 packet 接收完成；
6. `jpeg_taco` 进入 `blk_id && blk_vaddr` zero-copy 分支；
7. 编码完成后释放 Lease。

删除当前 Composite 路径中的：

- `composite_raw_buf_`；
- `nv12_copy`；
- 两次原始帧 memcpy。

### 11.2 硬件兼容验证

实施时必须先验证 TacoPro Display Buffer 的 block ID 是否满足
`jpeg_taco` 的 `inputPoolBlkId` 约定。

验证成功标准：

- `jpeg_taco` 不进入 `av_image_copy_to_buffer`；
- JPEG 输出正确；
- Buffer 在编码完成前不被复用；
- 连续运行无花屏、撕裂和 use-after-free。

### 11.3 一次 DMA copy 回退

如果 Display Buffer block ID 不能被 JPEG 编码器直接使用：

1. 为 Composite Encoder 分配一个可被 `jpeg_taco` 接受的专用 DMA Buffer；
2. Stitcher Buffer 通过硬件 copy 或一次 DMA copy 写入 staging Buffer；
3. AVFrame metadata 指向 staging Buffer；
4. 禁止复制到两个 `std::vector`；
5. 页面和状态接口必须显示当前模式是 `zero-copy` 或 `dma-staging`。

禁止静默回退到 OpenCV BGR 转换或三次 CPU copy。

## 12. Stitcher 固定节奏

### 12.1 通道输入语义

`channelWrite(channel_id, frame)` 改为更新该通道的 latest frame：

```text
新帧到达
  → 创建/替换该通道 ChannelFrameRef
  → 更新 sequence
  → 立即返回
```

不得等待 Render 开启新 round，不再使用
`written_this_round` 作为全通道 barrier。

### 12.2 Render 线程

Render 线程根据各通道当前 latest frame 生成下一张 Composite：

```text
Channel 0 有新帧 → 拼接新帧
Channel 1 无新帧 → 沿用上一次区域
Channel 2 有新帧 → 拼接新帧
```

Render 线程：

- 不等待全部通道；
- 不等待慢通道；
- 不积累每个通道的历史帧；
- 只处理各通道最新引用；
- 完成后发布新的 generation。

### 12.3 onTick

onTick 是唯一显示节拍：

```text
固定 tick
  ├─ 有新 Composite → 原子切换 current lease
  └─ 无新 Composite → 继续使用 current lease

current lease 始终提交给 IDS Display
```

当尚未生成第一张 Composite 时允许跳过；第一张生成后，每个 tick 都应有
可显示的 current lease。

### 12.4 Subscriber 隔离

onTick 禁止直接执行耗时 JPEG 编码或内存复制。

Subscriber 行为：

- IDS Subscriber：只把当前 Lease 提交给独立 latest-only DisplaySink；
- Preview Subscriber：只把 Lease 放入 latest-only 队列；
- onTick 不执行 display ioctl、vsync 或 JPEG 编码；
- DisplaySink 线程执行 framebuffer ioctl/vsync；
- onTick 不等待 Preview Encoder；
- 一个 Subscriber 的卡顿不能阻塞其他 Subscriber。

DisplaySink 跟不上时替换尚未提交的旧 Lease，只保留当前最新 Lease，并记录
`display_sink_drop_count` 和 ioctl/vsync 耗时。

### 12.5 generation 与重复帧

每张新 Composite 有唯一 generation。

- IDS 可在多个 tick 重复显示同一 generation；
- PreviewService 只对新 generation 执行一次 JPEG 编码；
- HTTP 需要固定发送节奏时复用已经编码好的 JPEG；
- 重复 tick 不得重复压缩完全相同的 NV12 数据。

## 13. WebUI 页面

### 13.1 Composite 模式

页面从后端布局接口获取真实 slot 映射：

```http
GET /api/preview/layout
```

响应至少包含：

- Composite 宽高；
- 行列数；
- slot index；
- channel_id；
- worker_id；
- Worker 名称；
- slot 像素坐标；
- Worker 状态。

页面不得再：

- 按 Worker 数组顺序推断 slot；
- 使用 `JPEG_PREVIEW` 判断参与通道；
- 使用单路 JPEG FPS 充当 Composite FPS。

Composite 配置与状态接口定义为：

```http
GET /api/preview/composite/config
PUT /api/preview/composite/config
GET /api/preview/status
```

`PUT /api/preview/composite/config` 只修改浏览器 Composite JPEG 的
`target_fps` 和 `quality`，不得修改 IDS/DISPLAY FPS。配置在 JPEG 编码前
生效。

### 13.2 双击进入单路

1. 每个 overlay cell 绑定自身 worker_id；
2. 双击后先发送创建 session 请求；
3. START 成功后再切换 UI；
4. START 失败时保留 Composite，并显示明确错误；
5. 单路画面使用返回的 session stream URL；
6. 再次双击发送 DELETE；
7. DELETE 完成后返回 Composite；
8. 页面卸载和路由切换时主动 DELETE 当前 session。

### 13.3 FPS 设置

IDS FPS 与浏览器预览 FPS 必须分开。

Composite 页面：

- IDS 目标 FPS；
- Stitcher 实际新帧 FPS；
- Composite JPEG 目标 FPS；
- Composite JPEG 实际编码 FPS；
- HTTP 实际发送 FPS；
- Composite JPEG quality；
- 当前输入模式：zero-copy 或 dma-staging。

单路页面：

- Worker 源 FPS；
- 单路 JPEG 目标 FPS；
- 单路 JPEG 实际编码 FPS；
- HTTP 实际发送 FPS；
- JPEG quality；
- encoder；
- session state。

### 13.4 限帧位置

预览目标 FPS 必须在 JPEG 编码之前执行：

```text
目标 10fps
  → 每秒最多向 JPEG 编码器提交 10 帧
  → 约编码 10 帧
  → HTTP 发送约 10 帧
```

废弃“先编码 30 帧、HTTP 最后只发送 10 帧”的行为。

IDS FPS 继续只控制显示/Stitcher，不被浏览器预览设置修改。

## 14. 指标

新增或修正以下指标：

### 14.1 Stitcher

- target_display_fps；
- render_fps；
- display_submit_fps；
- reused_tick_count；
- render_overrun_count；
- per_channel_input_fps；
- per_channel_latest_age_ms；
- composite_generation。

### 14.2 Composite Encoder

- target_encode_fps；
- encoded_fps；
- encode_send_avg_ms；
- encode_receive_avg_ms；
- zero_copy_frames；
- dma_staging_frames；
- dropped_before_encode；
- average_jpeg_size；
- latest_generation。

### 14.3 单路会话

- active_session_count；
- active_encoder_count；
- per_worker_source_fps；
- per_worker_encoded_fps；
- per_worker_sent_fps；
- per_worker_dropped_frames；
- encoder_start_ms；
- last_error。

### 14.4 HTTP

- active_clients；
- sent_fps；
- bytes_per_second；
- sink_write_avg_ms；
- disconnected_clients。

## 15. 错误处理

必须覆盖：

1. Worker 不存在；
2. Worker 未运行；
3. Worker 在 START 过程中停止；
4. JPEG 编码器不存在；
5. JPEG 硬件实例耗尽；
6. AVFrame metadata 不完整；
7. zero-copy block ID 不兼容；
8. session 重复创建或删除；
9. HTTP 客户端断开；
10. Server shutdown；
11. BufferPool shutdown；
12. Render 或编码线程异常退出。

错误不得通过捕获 `...` 后静默忽略。状态接口必须保留最后错误。

## 16. 并发与锁顺序

必须遵守：

1. REST 控制锁只保护 session/encoder map；
2. 编码器停止和 thread join 不在全局 map 锁内执行；
3. FrameTap 不获取长时间持有的控制锁；
4. latest-only queue 使用独立的小粒度锁或原子交换；
5. Subscriber 回调不得持有 subscribers 全局锁执行耗时操作；
6. Buffer Lease 释放不得回调到已经析构的 BufferPool；
7. Worker stop 与 session stop 必须可并发且幂等；
8. 禁止 detached 线程捕获裸 `this`。

## 17. 兼容与迁移

### 17.1 保留

- `JpegEncodeConsumer`；
- PreviewPlugin；
- QA/CLI 的 preview 子命令；
- 原有单路 MJPEG URL 形式；
- Composite MJPEG URL。

### 17.2 WebUI 迁移

- WebUI 不再创建静态 `JPEG_PREVIEW` Consumer；
- WorkerInfo 新增 `preview_defaults`，保存单路默认 fps、quality 和 encoder；
- 首次加载旧配置时，将 `JPEG_PREVIEW.config` 转换为 `preview_defaults`；
- 转换后从 `consumers_config` 删除该 `JPEG_PREVIEW` 条目并持久化新格式；
- 未完成持久化前也禁止把旧 `JPEG_PREVIEW` 传给 WebUI WorkerConfig；
- 前端 `previewableWorkers` 改为后端 layout slots；
- `/api/preview/fps` 标记为 deprecated；
- 新页面改用 session 和 Composite 配置接口。

## 18. 分阶段实施

### 阶段 1：单路按需会话与 flags

1. 修复每 Worker 独立 flags；
2. 增加 PreviewSessionManager；
3. 增加 FrameTap；
4. 增加 session REST API；
5. 增加真实 layout API；
6. 实现双击 START、再次双击 STOP；
7. 移除 WebUI 对静态 `JPEG_PREVIEW` 的依赖；
8. 板端验证只有显式 START 的 Worker 创建编码器。

### 阶段 2：Composite Lease 与零拷贝

1. 引入 StitchedFrameLease；
2. Composite latest-only Lease 队列；
3. AVFrame DMA metadata；
4. 验证 jpeg_taco zero-copy；
5. 实现一次 DMA staging 回退；
6. 删除两个 `std::vector` 原始帧缓冲；
7. 板端验证 Buffer 生命周期和长时间稳定性。

### 阶段 3：Stitcher 节拍与页面指标

1. channel latest-frame 模型；
2. 删除全通道 round barrier；
3. onTick 固定使用 current lease；
4. generation 去重编码；
5. Subscriber 隔离；
6. 上报真实阶段 FPS 和耗时；
7. 页面重新命名和展示所有设置；
8. 慢通道、停帧、编码过载回归。

每个阶段必须独立构建、部署、验证并向用户报告结果。得到用户确认后才进入
下一阶段，不得将三阶段同时修改后只做一次测试。

## 19. 测试策略

### 19.1 单元测试

- session 状态机；
- START/STOP 幂等性；
- 同 Worker 多 session 共享；
- 不同配置冲突；
- 最后 session 停止时释放编码器；
- latest-only 队列替换旧帧；
- 每 Worker flags 独立；
- generation 去重；
- Buffer Lease 最后引用归还 Pool。

### 19.2 集成测试

- 无会话时所有 Worker 均无单路 JPEG 编码；
- 双击 Worker 3 后只有 Worker 3 编码；
- 同时 START 多个 Worker；
- 硬件实例耗尽只拒绝新会话；
- STOP 单路不影响 IDS 和 Composite；
- Worker stop 自动清理 session；
- Composite zero-copy 分支；
- dma-staging 回退分支；
- 浏览器断开和页面切换；
- 多客户端共享同一 Worker。

### 19.3 板端节拍测试

- 所有通道同 FPS；
- 某一路 15fps、其他路 30fps；
- 某一路完全停帧；
- 某一路网络抖动；
- Render 偶发超过一个 tick；
- Preview 编码慢于 IDS；
- 多单路 JPEG 与 Composite 同时运行；
- 连续运行至少 30 分钟检查 BufferPool、线程和内存。

## 20. 验收标准

1. 未创建 session 时，单路 JPEG active encoder 数为 0；
2. START 一个 Worker 时只新增一个对应编码实例；
3. START 多个不同 Worker 时编码实例数量与被 START Worker 数一致；
4. STOP 最后一个 session 后对应编码实例和 BufferPool 被释放；
5. 任一单路失败不影响其他 Worker、Composite 和 IDS；
6. Composite 不再经过两个 heap vector；
7. zero-copy 支持时不调用输入 `av_image_copy_to_buffer`；
8. zero-copy 不支持时每帧最多一次 DMA staging copy；
9. 任一路停帧时 IDS 仍按固定目标节拍提交当前 Composite；
10. Render 未产生新帧时复用上一 Composite，不跳过整个显示节拍；
11. 相同 Composite generation 不重复执行 JPEG 压缩；
12. 页面双击进入指定 Worker，第二次双击退出；
13. 页面 slot 与后端真实 worker_id/channel_id 一致；
14. 页面目标 FPS、编码 FPS、发送 FPS名称和实际含义一致；
15. 旧 QA/CLI preview 功能保持可用。

## 21. 已确认设计决策

1. 采用方案 3；
2. 单路预览由 WebUI 双击触发；
3. 再次双击退出单路预览；
4. 控制消息来自 WebUI REST API；
5. 使用显式 START/STOP 生命周期；
6. 允许任意多个 Worker 被显式 START；
7. WebUI 单路预览不由静态 `JPEG_PREVIEW` Consumer 驱动；
8. IDS FPS 与浏览器预览 FPS 分离；
9. Stitcher 目标是固定 tick 使用各通道最新画面，不等待慢通道；
10. 代码实现必须按阶段报告验证结果，并在用户确认后继续下一阶段。
