# procedure_error 调查：Procedure「把用例名当 Input」

- 调查日期: 2026-07-18
- 结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_234050`
- 表面 FAIL: **5** 条（TC-2873 / 2874 / 2875 / 2948 / 2949）
- 同根因受影响飞书用例: **66** 条（含大量假 PASS / 假 Expected-Fail）

## 结论（一句话）

不是参数顺序写反，而是飞书 Procedure 引用了 **`qa_cases venc` 未注册的预定义名**；CLI 把未知 positional 当成输入路径，真实 yuv 被忽略。名字里带 `nv12` 时打不开文件 → 记为 procedure_error；名字不带 `nv12` 时被 Auto-format 偷换成 `1920x1080_nv12.yuv` → **假 PASS**。

## 复现链

飞书写法（与大量真 PASS 相同）：

```text
qa_cases venc -p -S -M 28 -N 0.85 <profile> /usr/data/qa/<WxH>_nv12.yuv
```

CLI 解析（`VencPlugin::handlePreActions`）：

1. positional 若在预定义表 → 加载参数；
2. 否则若 `input_path_` 为空 → **整段当文件路径**；
3. 已有 input 后，后续 path **直接丢弃**。

因此未知 profile 时：

| 现象 | 原因 |
|------|------|
| `Input: spec22_jpeg_nv12_144x96` | 用例名被当成路径 |
| `CustomEncode: 1920x1080` | 未命中预定义，走默认宽高 |
| 真实 `/usr/data/qa/144x96_nv12.yuv` 未用 | 第二个 positional 被忽略 |

对照：`spec22_jpeg_yuv420p_96x32` 在矩阵里 → TC-1743 等同写法 PASS。

## 根因：JPEG 矩阵缺分辨率 + boundary 未实现

`test_cases/venc/VencPlugin.cpp`：

- H.264 `kSpec21Resolutions`：**有** 144x96 / 320x240 / 640x480
- JPEG `kSpec22JpegResolutions`：**仅** 96x32, 512x512, 1280x720, 1920x1080, 2560x1440, 3840x2160, 8192x8192
- 全文件 **无** `boundary_jpeg_*` 注册

板端 `qa_cases venc -l` 可见 `spec22_jpeg_nv12_96x32` 等，**没有** `spec22_jpeg_nv12_144x96` / `boundary_jpeg_nv12_96x8192`。

## 为何只有 5 条 FAIL，其余 61 条「过了」

同属未注册名，分叉在 Auto-format（`buildEncodeConfigInternal`）：

- 默认 CustomEncode `input_format=nv12`
- 若「路径」字符串已含 `nv12` → 认为格式已匹配 → **不替换** → `fopen(用例名)` 失败  
  → TC-2873/2874/2875/2948/2949（Expectation=PASSED）→ **FAIL / procedure_error**
- 若路径字符串不含 `nv12`（yuv420p/rgb888/…）→ 搜到 `/usr/data/ffmpeg/1920x1080_nv12.yuv` 并替换  
  → 实际测的是 **1080p NV12**，不是飞书写的分辨率/格式  
  → 约 **46** 条 **假 PASS**（含 TC-2870–2872、2876–2914、2960、2961 等）
- `boundary_jpeg_nv12_*` 且 Expectation=FAIL：打不开文件也算「失败」→ **假 Expected-Fail PASS**（约 14 条，如 TC-2922）
- TC-2962（qneg1）：被换成 1080p 后意外质量过线 → Unexpected Pass（记在 case_fail）

## 责任分层

| 层 | 问题 | 性质 |
|----|------|------|
| 飞书 Procedure | 引用代码不存在的 profile | 数据/实现不同步 |
| `qa_cases venc` CLI | 未知预定义名静默当路径、忽略第二参数 | 可观测性差 |
| Auto-format | 把用例名当路径再「纠正」成 1080p | 掩盖错误，制造假 PASS |
| 编码器 / 硬件 | 本批 5 条未测到真实编码路径 | **不是**硬件问题 |

## 修复建议

1. **代码（推荐）**：`kSpec22JpegResolutions` 补齐 144x96 / 320x240 / 640x480；按需实现并注册 `boundary_jpeg_*`。
2. **飞书**：未实现前改 Procedure 为已有 profile，或显式 `-i <yuv> -c jpeg -W/-H -f ...`。
3. **CLI 加固**：positional 像预定义名但不在表中时 **直接报错退出**，禁止当路径；Auto-format 勿对明显非路径字符串生效。
4. **报告修订**：本轮 PASS=288 中，至少上述假 PASS / 假 Expected-Fail 应降级为无效结果；procedure_error 表面 5 条，实质同根因 **66** 条。

## 证据摘录

```text
# TC-2873（可见 FAIL）
CustomEncode: 1920x1080
Input: spec22_jpeg_nv12_144x96
无法打开文件: spec22_jpeg_nv12_144x96

# TC-2870（假 PASS）
[venc] Auto-format resolve: spec22_jpeg_yuv420p_144x96 → /usr/data/ffmpeg/1920x1080_nv12.yuv
Input: /usr/data/ffmpeg/1920x1080_nv12.yuv
ENC_COMPARE done: ... pass=1  → FINAL_VERDICT=PASS
```
