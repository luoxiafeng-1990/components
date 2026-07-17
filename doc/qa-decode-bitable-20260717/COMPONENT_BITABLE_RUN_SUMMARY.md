# COMPONENT_BITABLE_RUN_SUMMARY — QA_DECODE

- 结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_125226`
- 执行用例: 136（飞书 QA_DECODE 可执行集）
- 最终（首轮 P5）: **PASS=123 / FAIL=13**
- **A 类 RTSP 复测后有效状态**: 原 7 条 host_env 中 **4 条转 PASS**；**3 条**因摄像头能力不足 blocked（见下节）
- 当前仍关注 FAIL: procedure_error×4 + case_fail×2 + hardware_limit(摄像头能力)×3
- Flaky(retested_pass): 6 条 — TC-1455, TC-1580, TC-2319, TC-2320, TC-2321, TC-2325
- P5: 见 `LOG_REVIEW_REPORT.md`；A 类复测目录见下节

## FAIL 归因汇总（含 A 类复测）

| FAIL_REASON | 数量 | 说明 |
|-------------|------|------|
| ~~host_env_error~~ → 已复测 | 原7→4PASS+3能力限制 | TC-1456/1457/1458/2317 复测 PASS；TC-1459/2322/1557 摄像头不支持目标格式/分辨率 |
| procedure_error | 4 | Expectation/Procedure 矛盾或 `-m -1`: TC-1560, TC-1562, TC-1574, TC-2977 |
| case_fail | 2 | multithread 收尾挂死: TC-1582, TC-1583 |
| hardware_limit | 3 | 实验室海康无 4K / 无 MJPEG 主码流: TC-1459, TC-2322, TC-1557 |

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
| TC-1459 | h264 3840×2160 cbr | **FAIL / blocked** | 能力集无 4K → `hardware_limit`（未跑板端） |
| TC-2322 | h264 3840×2160 vbr | **FAIL / blocked** | 同上 |
| TC-1557 | mjpeg 7680×4320 | **FAIL / blocked** | 海康主码流无 MJPEG → `hardware_limit` |

复测汇总：**4 PASS / 3 blocked(hardware_limit)**。可配参范围内 A 类环境问题已闭环；剩余 3 条需 4K/MJPEG 能力摄像头或改 Procedure。

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

### C. case_fail — 被测组件问题（2 条）

| TC-ID | HOST | exit | Procedure | 日志事实 | 结论 |
|-------|------|------|-----------|---------|------|
| TC-1582 | 56.92 | 137 | `vdec multithread_4` + 1080p h264 | 帧级 PTS-match PSNR≈100，近 Frame100 后反复 `Max timeout count reached: 10`，停车间挂起，无完整 Compare 汇总，SIGKILL | 多线程 COMPARE **收尾/停车间挂死** |
| TC-1583 | 56.140 | 137 | `vdec multithread_8` + 1080p h264 | 同上；Frame100 出现 PTS 不一致缓存后挂死 | multithread_8 同类收尾挂死 |

**建议（P7）**：排查 `BufferConsumerService` / `MultiWorker` 在 multithread COMPARE 结束时的 stop 路径（Max timeout 后无法退出）；对比单路 COMPARE 正常收尾路径。

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

- 执行总数: 136 = PASS 123 + FAIL 13
- case_fail 需 P7 关注: TC-1582, TC-1583（multithread 收尾挂死）
- 飞书数据需修正: TC-1560/1562/1574 Expectation；TC-2977 Procedure(`-m -1`)与空 Expectation
- RTSP 基建: 57.225 dead、多摄像头配参失败导致 7 条 host_env_error
