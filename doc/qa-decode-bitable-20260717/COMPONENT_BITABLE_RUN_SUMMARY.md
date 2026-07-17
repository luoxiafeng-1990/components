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

## Flaky 双套日志对比（P5.2bis）

| TC-ID | 转 PASS 轮次 | 状态反转物理归因 |
|-------|-------------|------------------|
| TC-1455 | R2 | 换主机+RTSP 119 码流匹配 |
| TC-1580 | R3 | 轮间清理/换板后重跑通过 |
| TC-2319 | R3 | RTSP 配参稳定后通过 |
| TC-2320 | R2 | 换板+RTSP 119 |
| TC-2321 | R4 | 多轮 RTSP/板端重试 |
| TC-2325 | R4 | 中途 VPU FATAL 后最终 Compare PASSED |

## 逐条 FAIL 结果

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
