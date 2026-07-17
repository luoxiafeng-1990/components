# QA_DECODE 素材缺口统计与补齐（2026-07-17）

板端核查主机：`192.168.57.113`，目录 `/usr/data/qa/`。

## 1. 统计（Procedure 引用的文件素材）

| 项 | 数量 |
|----|------|
| Procedure 引用的唯一文件路径 | 33 |
| 核查前板端已有 | 25 |
| 核查前缺失 | **8** |
| 补齐后缺失 | **0** |
| RTSP 用例（需直播 URL，非文件） | 34 |

## 2. 核查前缺失清单（已生成并上传）

| 文件 | 引用用例 | 可执行 | 来源 |
|------|----------|--------|------|
| `48x48.jpg` | TC-3200, TC-3202 | 是 | 由板端 `1920x1080.jpg` 缩放生成 |
| `320x240.jpg` | TC-1537, TC-1546 | 是 | 同上 |
| `640x480.jpg` | TC-1538, TC-1547 | 是 | 同上 |
| `1280x720.jpg` | TC-1540, TC-1549 | 是 | 同上 |
| `3840x2160.jpg` | TC-1544, TC-1553 | 是 | 同上 |
| `48x48_60fps.mjpeg` | TC-3201, TC-3203 | 否 | 由板端 `640x480_h264_60fps.mp4` 缩放转 MJPEG |
| `640x480_60fps.mjpeg` | TC-1539, TC-1548 | 否 | 由板端 `640x480_h264_60fps.mp4` 转 MJPEG |
| `1920x1080_60fps.mjpeg` | TC-1542, TC-1551 | 否 | 由板端 `1920x1080_h264_60fps.mp4` 转 MJPEG |

额外上传（正确 60fps 时间基容器）：
- `48x48_60fps.avi` / `640x480_60fps.avi` / `1920x1080_60fps.avi`（`r_frame_rate=60/1`，同源 mp4）

说明：已按用户要求弃用 testsrc 合成内容；MJPEG 均来自板端现有 60fps mp4。裸 `.mjpeg` 可能丢帧率元数据，需要时可用对应 `.avi`。

## 3. 网络下载说明

- 已尝试 Wikimedia / picsum 拉取公开图片；Wikimedia SSL 失败，picsum 成功备用。
- 精确分辨率 JPEG 最终以板端已有 `1920x1080.jpg` + ffmpeg 缩放生成为准（更贴合现有素材风格）。
- 公开编解码测试集参考：[Xiph Derf Test Media](https://media.xiph.org/video/derf/)（多为 Y4M，体积大，适合后续补标准序列，非本次 8 个缺口的最优路径）。

## 4. 「用例名 vs Description vs Procedure」以谁为准

**统一原则（与此前你确认的一致）：**

1. **有分辨率/编解码语义的「用例名（Test case）」为准**  
   - Procedure 必须跟着用例名改。  
   - 例：TC-1571 用例名 `h265_1080p_crop_scale` → Procedure 应使用 `1920x1080_h265_30fps.mp4`（板端已有），而不是 h264。  
   - 按「用例名 vs Procedure」看 TC-1571：**目前不一致，应以用例名为准改 Procedure。**

2. **用例名不含分辨率意图时（如 `multi_pp_crop1`）**  
   - 以 **Procedure 实际命令 + 输入文件** 为可执行真相。  
   - **Description 应改成与 Procedure 一致**（不要反过来改 Procedure 去迁就过时描述）。  
   - 对超规格意图用例：若 Description 写 32768 而文件是 720p/4K，需二选一统一：  
     - A）改 Description/Expectation 对齐真实输入；或  
     - B）改 preset/输入以真正测 32768（并 Expectation=FAILED）。  
   - 你此前已确认超规格 Expectation=FAILED；文件仍是合法分辨率时，**Description 更应改写为“preset 配置意图/当前输入”的准确表述**，避免和 Procedure 打架。

3. **按「用例名 vs Procedure」复查那 8+1：**  
   - 8 条 PP：用例名无分辨率数字 → **不算用例名硬冲突**；冲突在 Description。  
   - 1 条 TC-1571：用例名含 h265 → **算硬冲突，用例名对、Procedure 错。**
