# COMPONENT_BITABLE_RUN_SUMMARY — QA_DECODE

- 结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_125226`
- 执行用例: 136（飞书 QA_DECODE 可执行集）
- 最终（以 result.txt + P5 日志为准）: **PASS=123 / FAIL=13**
- MULTI_ROUND_SUMMARY 原记录: PASS=124 / FAIL=12（TC-1557 误标 PASS，已纠正）
- Flaky(retested_pass): 6 条 — TC-1455, TC-1580, TC-2319, TC-2320, TC-2321, TC-2325
- 主机: 空闲探测并行（见 idle_hosts.txt）
- Procedure 特征: `qa_cases vdec` / `pp` / RTSP / multithread / display stitcher
- P5: 13 条 FAIL 已逐条读 log；见 `LOG_REVIEW_REPORT.md`
- 多轮: max_rounds=4；Round1 118P/18F → 最终 123P/13F（纠正后）

## FAIL 归因汇总

| FAIL_REASON | 数量 | 说明 |
|-------------|------|------|
| host_env_error | 7 | RTSP 摄像头码流与期望编码/分辨率不匹配（含 TC-1557）: TC-1456, TC-1457, TC-1458, TC-1459, TC-1557, TC-2317, TC-2322 |
| procedure_error | 4 | 飞书 Expectation/Procedure 矛盾或 `-m -1`/空 Expectation: TC-1560, TC-1562, TC-1574, TC-2977 |
| case_fail | 2 | multithread_4/8 COMPARE 收尾挂死: TC-1582, TC-1583 |

---

## FAIL 逐条原因分析（P5 日志）

### A. host_env_error — RTSP 码流不匹配（7 条）

共性：Procedure 期望特定 **h264/mjpeg + 分辨率**，实际摄像头拉到 **hevc + 其它分辨率** → `Failed to open codec (FFmpeg: Invalid argument)` → Build Phase 失败。多数 exit=137 为挂死后 SIGKILL，根因是配参/回退失败，非硬解算法缺陷。背景：海康 `192.168.57.225` 已标 dead，后续回退摄像头常无法落到目标配置。

| TC-ID | HOST | exit | 期望 | 实际码流 | 日志关键错误 | 结论 |
|-------|------|------|------|----------|--------------|------|
| TC-1456 | 56.86 | 137 | h264 1280×720 vbr | 57.243 hevc 2560×1440 | Codec Mismatch；open codec 失败 | 摄像头未配成目标参数 |
| TC-1457 | 56.86 | 137 | h264 1920×1080 cbr | 57.223 hevc 1280×720 | 同上 | dead 摄像头回退后码流不符 |
| TC-1458 | 56.39 | 137 | h264 1920×1080 vbr | 57.224 hevc 3200×1800 | 同上 | 配参未落到 1080p h264 |
| TC-1459 | 56.39 | 137 | h264 3840×2160 cbr | 57.253 hevc 1280×720 | 同上 | 无法提供 4K h264 |
| TC-1557 | 56.92 | -1 | mjpeg 7680×4320 | 57.224 hevc 3200×1800 | 日志在 Frame4 截断，无 Compare 汇总 | 超大 MJPEG 配参失败；汇总曾误标 PASS，已纠正 |
| TC-2317 | 56.133 | 137 | h264 1280×720 cbr（双 ch） | 57.253 hevc 1280×720 | Codec 为 hevc 非 h264 | 编码格式不匹配 |
| TC-2322 | 56.56 | 137 | h264 3840×2160 vbr（双 ch） | 57.243 hevc 2560×1440 | 同上 | 未提供期望 4K h264 |

**建议**：修复/更换故障摄像头；配参回读失败勿回退到参数不符的 URL；dead 摄像头持久化过滤已部分落地，需保证池内仍有可配到目标 (codec,w,h,fps) 的设备。

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
