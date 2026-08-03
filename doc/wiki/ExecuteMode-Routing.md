# ExecuteMode 路由与调用链

## `qa_cases vdec ...` 调用链

```mermaid
sequenceDiagram
  participant U as 用户 CLI
  participant M as test_module_main
  participant P as VdecPlugin(+伴随)
  participant E as ExecuteMode
  participant S as BufferConsumerService
  participant L as ProductionLine

  U->>M: qa_cases vdec --file v.mp4 display
  M->>P: registerOptions / parse
  M->>P: handlePreActions
  M->>P: applyTo(shared WorkerConfig)
  M->>P: buildPipelineConfigs(shared)
  P-->>M: 1..N WorkerConfig
  M->>M: inheritCompanionSettings + buildConsumeFlags
  alt CHANNEL_COMPARE
    M->>E: channelCompare
  else 单路 ENCODE + display
    M->>E: venc::runEncodeDecodeDisplay
  else PARALLEL COMPARE (偶数组)
    M->>E: 多线程 compare(pair)
  else COMPARE enabled
    M->>E: compare(configs)
  else configs.size > 1
    M->>E: parallel / BATCH single
  else
    M->>E: single
  end
  E->>S: start / startMultiWorkerCompare / ...
  S->>L: VideoProductionLine / MultiWorker
  L-->>U: ConsumeResult + exit code
```

## 模式决策树

```mermaid
flowchart TD
  Start[pipeline_configs 就绪] --> CC{channel_compare?}
  CC -->|是| Ch[channelCompare]
  CC -->|否| Enc{单路 ENCODE + display<br/>且非 compare?}
  Enc -->|是| EDD[runEncodeDecodeDisplay]
  Enc -->|否| PC{compare 且 size>2<br/>且偶数?}
  PC -->|是| PCmp[PARALLEL COMPARE 多线程]
  PC -->|否| OV{opencv.enable?}
  OV -->|是| Sing1[single]
  OV -->|否| Cmp{compare_enabled?}
  Cmp -->|是| CmpE[ExecuteMode::compare<br/>PEER 或 SOURCE_REF]
  Cmp -->|否| Many{size > 1?}
  Many -->|是| Batch{save 且无 display/compare?}
  Batch -->|是| B[BATCH 串行 single]
  Batch -->|否| Par[parallel]
  Many -->|否| Sing2[single]
```

## 模式对照

| 模式 | 触发条件（摘要） | 运行时 |
|------|------------------|--------|
| SINGLE | 1 路，非 compare | `VideoProductionLine` + flags 消费者 |
| PARALLEL | N 路，非 batch | 多路 `startProductionLine` |
| BATCH | N 路 + save 且无 display/compare | 串行 single |
| COMPARE PEER | psnr/ssim + target=PEER | MultiWorker ONE_TO_MANY + sync |
| COMPARE SOURCE_REF | psnr/ssim + target=SOURCE_REF（或 venc 默认） | `runEncodeQualityCompare` |
| CHANNEL_COMPARE | `enable_channel_compare` | `channelCompare` |
| PARALLEL COMPARE | compare 且 configs 偶数且 >2 | 每对 hw/sw 一线程 |
| ENCODE→DECODE→DISPLAY | 单路 encode + display | `runEncodeDecodeDisplay` |

## 关键文件

- `test_cases/test_module_main.cpp` — 路由主开关
- `test_cases/common/ExecuteMode.{hpp,cpp}` — 模式实现
- `include/consumptionline/.../BufferConsumerService` — 门面
