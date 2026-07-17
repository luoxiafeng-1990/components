# LOG_REVIEW_REPORT — QA_DECODE

- 结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_125226`
- 生成时间: 2026-07-17 14:48:57
- 复查总数: **136**（本轮执行全部 case）
- result.txt 扫描: PASS=123 / FAIL=13
- MULTI_ROUND_SUMMARY 原记录: PASS=124 / FAIL=12
- 一致数: 135
- 不一致数: 1（已按日志修正）

## 不一致项详情

| TC-ID | 原判定 | 日志实际 | 修正后判定 | 修正原因 |
|-------|--------|----------|------------|----------|
| TC-1557 | MULTI_ROUND_SUMMARY=PASS | result.txt+log=FAIL (exit=-1, 码流不匹配且日志截断) | FAIL / host_env_error | 汇总误标 PASS；以日志为准纠正 |

## FAIL 归因分类统计（基于 P5 逐条读 log）

| FAIL_REASON | 数量 | TC-ID |
|-------------|------|-------|
| host_env_error | 7 | TC-1456, TC-1457, TC-1458, TC-1459, TC-1557, TC-2317, TC-2322 |
| procedure_error | 4 | TC-1560, TC-1562, TC-1574, TC-2977 |
| case_fail | 2 | TC-1582, TC-1583 |

## 逐条 FAIL 日志结论摘要

| TC-ID | exit | 根因摘要 | FAIL_REASON |
|-------|------|----------|-------------|
| TC-1456 | 137 | 期望 h264 720p，实得 hevc 2560x1440，open codec 失败 | host_env_error |
| TC-1457 | 137 | 期望 h264 1080p cbr，实得 hevc 720p（57.223） | host_env_error |
| TC-1458 | 137 | 期望 h264 1080p vbr，实得 hevc 3200x1800 | host_env_error |
| TC-1459 | 137 | 期望 h264 4K cbr，实得 hevc 720p | host_env_error |
| TC-1557 | -1 | 期望 mjpeg 8K，实得 hevc 3200x1800，日志截断 | host_env_error |
| TC-1560 | 0 | Expectation 超规格 FAIL，Procedure 却是 720p 且实际 PASS | procedure_error |
| TC-1562 | 0 | 同上（multi_pp_crop4 + 720p） | procedure_error |
| TC-1574 | 0 | Expectation 写超 4096 上限，实为 3840x2160 合法分辨率且 PASS | procedure_error |
| TC-1582 | 137 | multithread_4 帧级 PSNR 正常，收尾 Max timeout 挂死被杀 | case_fail |
| TC-1583 | 137 | multithread_8 同上收尾挂死 | case_fail |
| TC-2317 | 137 | 期望 h264 720p cbr，实得 hevc 720p | host_env_error |
| TC-2322 | 137 | 期望 h264 4K vbr，实得 hevc 2560x1440 | host_env_error |
| TC-2977 | -1 | Procedure `-m -1` 无限循环 + Expectation 空 + TIMEOUT=60 | procedure_error |

## Flaky（retested_pass）双套对比说明

共 6 条：TC-1455, TC-1580, TC-2319, TC-2320, TC-2321, TC-2325。
本轮未按规范落盘 `qa_cases_full_roundN.log`（轮次备份缺失），对比依据为 scheduler 多次执行时间线 + 最终 PASS 日志：

| TC-ID | 转 PASS 轮次 | 状态反转物理归因 |
|-------|-------------|------------------|
| TC-1455 | R2 | 换板/换 RTSP（57.119）后码流匹配，COMPARE PSNR=100 |
| TC-1580 | R3 | 多轮重试 + 残留清理后本地 multithread 相关路径恢复 |
| TC-2319 | R3 | RTSP 配参稳定后（57.119）双通道 COMPARE 通过 |
| TC-2320 | R2 | 换板 + RTSP 119 码流匹配后通过 |
| TC-2321 | R4 | 多轮 RTSP/板端重试后最终 PSNR=100 |
| TC-2325 | R4（共6次） | 中途曾 FATAL driver dead，最终仍 Compare 70 帧 PASSED；属 RTSP/VPU 瞬时故障后恢复 |

## P5 执行记录

- P5.1: 扫描 result.txt 得到 FAIL 13 条（含 TC-1557）
- P5.2: 13 条 qa_cases_full.log 均已 Read 工具实际读取
- P5.3: 13 条 result.txt 已写入三段式判定与 FAIL_REASON
- P5.4: 本文件
