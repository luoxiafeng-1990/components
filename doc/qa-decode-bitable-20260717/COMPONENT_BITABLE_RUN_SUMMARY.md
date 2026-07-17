# COMPONENT_BITABLE_RUN_SUMMARY — QA_DECODE

- 结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_125226`
- 执行用例: 136（飞书 QA_DECODE 可执行集）
- 最终（首轮 P5）: **PASS=123 / FAIL=13**
- **A 类 RTSP 复测**: 原 7 条 host_env → **4 PASS**（720p/1080p）+ **2 条改归 case_fail**（4K 大华已通，停机挂死）+ **1 条仍 hardware_limit**（mjpeg 8K）
- 当前仍关注 FAIL: procedure_error×4 + case_fail×4（含 4K teardown）+ hardware_limit×1（TC-1557）
- Flaky(retested_pass): 6 条 — TC-1455, TC-1580, TC-2319, TC-2320, TC-2321, TC-2325
- P5: 见 `LOG_REVIEW_REPORT.md`；A/4K 复测目录见下节

## FAIL 归因汇总（含 A 类 + 大华 4K 复测）

| FAIL_REASON | 数量 | 说明 |
|-------------|------|------|
| ~~host_env_error~~ → 已复测闭环 | 原7 | 720p/1080p×4 PASS；4K×2 环境已通改归 case_fail；mjpeg8K×1 仍无设备 |
| procedure_error | 4 | Expectation/Procedure 矛盾或 `-m -1`: TC-1560, TC-1562, TC-1574, TC-2977 |
| case_fail | 4 | multithread 收尾挂死 TC-1582/1583；**RTSP 4K compare 停机挂死** TC-1459/2322 |
| hardware_limit | 1 | 实验室无 MJPEG 主码流 7680×4320: TC-1557 |

---

## FAIL 逐条原因分析（P5 日志）

### A. host_env_error — RTSP 码流不匹配（原 7 条）→ 已专项复测

共性（首轮）：Procedure 期望特定 **h264/mjpeg + 分辨率**，调度器仅配 `57.225` 且失败后**回退未配参 URL** → 拉到 hevc 其它分辨率 → `Failed to open codec`。

#### A 类复测（2026-07-17 15:37，`--skip-compile-deploy`）

结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_aclass_retest`

前置动作：
1. 实测海康能力集：`243/225` → 1280×720 / 1920×1080 / 2560×1440；`223` → 仅到 1080p；**均无 3840×2160**；主码流**无 MJPEG**；最高帧率 **25fps**
2. 预配通：`243`=H.264 1280×720，`223/225`=H.264 1920×1080（下发 fps=25）
3. 调度器改为多海康候选配参 + **禁止回退未配参 URL**（不支持则 blocked）

| TC-ID | 期望 | 复测结果 | 证据 |
|-------|------|----------|------|
| TC-1456 | h264 1280×720 | **PASS** | RTSP=`57.243`，码流 h264 1280×720，Compare 295 帧 PSNR=100 |
| TC-2317 | h264 1280×720 双 ch | **PASS** | 同上，Compare 295 帧 PSNR=100 |
| TC-1457 | h264 1920×1080 cbr | **PASS** | RTSP=`57.243` 配成 1080p h264，Compare 91 帧 PSNR=100 |
| TC-1458 | h264 1920×1080 vbr | **PASS** | 同上，Compare 101 帧 PSNR=100 |
| TC-1459 | h264 3840×2160 cbr | 海康 blocked | 海康无 4K → 见下节大华 4K 复测 |
| TC-2322 | h264 3840×2160 vbr | 海康 blocked | 同上 |
| TC-1557 | mjpeg 7680×4320 | **FAIL / blocked** | 全库无 MJPEG 主码流 8K → `hardware_limit` |

海康复测汇总：**4 PASS / 3 blocked**。随后接入大华 `57.222` 做 4K 专项复测（下节）。

#### A2. 大华 4K 复测（2026-07-17 16:23，`--skip-compile-deploy`）

结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_4k_retest`

前置动作：
1. 飞书测试资源表确认 `192.168.57.222`（DH-IPC-HDW4843T-A，`admin/admin6666`）支持主码流 H.264 3840×2160
2. `compliant_run.py` 新增大华 CGI 配参/回读/`dahua_rtsp_url`；禁用本机代理劫持局域网请求
3. 调度器 `_DAHUA_CAMERA_CAPS` 接入 222；配参顺序海康→大华；禁止回退未配参 URL

| TC-ID | 期望 | 复测结果 | 证据 |
|-------|------|----------|------|
| TC-1459 | h264 3840×2160 cbr | **FAIL (`case_fail`)** | 大华配参+回读 OK；板端 `Resolution: 3840x2160` / `Codec: h264`；Compare **259** 帧多 PSNR=100 → `Max timeout count reached: 10` → `Waiting for free buffer` 停机挂死 → timeout/SIGKILL |
| TC-2322 | h264 3840×2160 vbr 双 ch | **FAIL (`case_fail`)** | 同上；Compare **251** 帧 PSNR=100 后同路径挂死 |
| TC-1557 | mjpeg 7680×4320 | 未重测 | 仍无设备可提供该主码流 |

结论：
- **摄像头环境对 4K 已闭环**（大华 222 可稳定提供目标码流）
- 失败根因与 C 类相同：COMPARE 在 Max timeout 后 **teardown 挂死**，属产品侧 `case_fail`，不是 host_env
- 调度器同组 RTSP 补扫曾误杀/删日志，4K 复测最终用单板串行确认上述模式

### B. procedure_error — 飞书用例数据问题（4 条）

| TC-ID | HOST | exit | Procedure 实际行为 | Expectation | 日志事实 | 结论 |
|-------|------|------|-------------------|-------------|---------|------|
| TC-1560 | 56.132 | 0 | `pp multi_pp_crop2` + **1280×720** mp4 | FAILED；超出硬解上限 4096×2160 | Compare 96 帧 PSNR=100 **PASSED** | Unexpected Pass：Expectation 与输入分辨率矛盾 |
| TC-1562 | 56.93 | 0 | `pp multi_pp_crop4` + **1280×720** mp4 | 同上 | 同上 PASSED | 同上 |
| TC-1574 | 56.56 | 0 | `pp multi_pp_scale3` + **3840×2160** mp4 | 同上（超上限） | 同上 PASSED | 3840×2160 ≤ 4096×2160，Expectation 误标超规格 |
| TC-2977 | 56.135 | -1 | `vdec ... -t 16 --loop -m -1 display ... grid` | （空） | stitcher 持续跑、多路 missed frame，TIMEOUT=60 被打断 | `-m -1` 无限循环无退出条件 + 空 Expectation + 超时过短 |

**建议**：
- TC-1560/1562：若要测「超硬解上限应失败」，Procedure 须换成 >4096×2160 素材；否则 Expectation 改为 PASSED。
- TC-1574：Expectation 改为 PASSED（或换真正超上限素材）。
- TC-2977：去掉 `-m -1`，改为有限帧/有限秒；补全 Expectation；TIMEOUT 与场景匹配。

### C. case_fail — 被测组件问题（4 条）

| TC-ID | HOST | exit | Procedure | 日志事实 | 结论 |
|-------|------|------|-----------|---------|------|
| TC-1582 | 56.92 | 137 | `vdec multithread_4` + 1080p h264 | 帧级 PTS-match PSNR≈100，近 Frame100 后反复 `Max timeout count reached: 10`，停车间挂起，无完整 Compare 汇总，SIGKILL | 多线程 COMPARE **收尾/停车间挂死** |
| TC-1583 | 56.140 | 137 | `vdec multithread_8` + 1080p h264 | 同上；Frame100 出现 PTS 不一致缓存后挂死 | multithread_8 同类收尾挂死 |
| TC-1459 | 56.132 | -1 | `vdec rtsp_h264_3840x2160_30_cbr` | 大华 4K 码流正确；259 帧 PSNR≈100 后 Max timeout → Waiting for free buffer 挂死 | RTSP 4K COMPARE **同类停机挂死** |
| TC-2322 | 56.132 | 137 | `vdec rtsp_h264_3840x2160_30_vbr` 双 ch | 同上；251 帧后同路径挂死 | 同上 |

**建议（P7）**：排查 `BufferConsumerService` / `MultiWorker` 在 COMPARE 结束时的 stop 路径（Max timeout 后无法退出）；覆盖 multithread 与 RTSP 4K 两条触发路径；对比 720p/1080p RTSP 正常收尾路径。

---

## Flaky 双套日志对比（P5.2bis）

| TC-ID | 转 PASS 轮次 | 状态反转物理归因 |
|-------|-------------|------------------|
| TC-1455 | R2 | 换主机+RTSP 119 码流匹配 |
| TC-1580 | R3 | 轮间清理/换板后重跑通过 |
| TC-2319 | R3 | RTSP 配参稳定后通过 |
| TC-2320 | R2 | 换板+RTSP 119 |
| TC-2321 | R4 | 多轮 RTSP/板端重试 |
| TC-2325 | R4 | 中途 VPU FATAL 后最终 Compare PASSED |

## 逐条 FAIL 结果一览

| TC-ID | FINAL | EXIT | HOST | FAIL_REASON |
|-------|-------|------|------|-------------|
| TC-1456 | FAIL | 137 | 192.168.56.86 | host_env_error |
| TC-1457 | FAIL | 137 | 192.168.56.86 | host_env_error |
| TC-1458 | FAIL | 137 | 192.168.56.39 | host_env_error |
| TC-1459 | FAIL | 137 | 192.168.56.39 | host_env_error |
| TC-1557 | FAIL | -1 | 192.168.56.92 | host_env_error |
| TC-1560 | FAIL | 0 | 192.168.56.132 | procedure_error |
| TC-1562 | FAIL | 0 | 192.168.56.93 | procedure_error |
| TC-1574 | FAIL | 0 | 192.168.56.56 | procedure_error |
| TC-1582 | FAIL | 137 | 192.168.56.92 | case_fail |
| TC-1583 | FAIL | 137 | 192.168.56.140 | case_fail |
| TC-2317 | FAIL | 137 | 192.168.56.133 | host_env_error |
| TC-2322 | FAIL | 137 | 192.168.56.56 | host_env_error |
| TC-2977 | FAIL | -1 | 192.168.56.135 | procedure_error |

## 统计核对

- 执行总数: 136 = PASS 123 + FAIL 13（首轮）
- A 类闭环后有效：PASS 127（+4）/ 仍 FAIL 9（procedure×4 + case_fail×4 + hardware_limit×1）
- case_fail 需 P7 关注: TC-1582/1583（multithread）+ TC-1459/2322（RTSP 4K teardown）
- 飞书数据需修正: TC-1560/1562/1574 Expectation；TC-2977 Procedure(`-m -1`)与空 Expectation
- RTSP 基建: 海康多机配参 + 大华 222 4K 配参已接入；禁止回退未配参 URL；TC-1557 仍缺 MJPEG 8K 设备
