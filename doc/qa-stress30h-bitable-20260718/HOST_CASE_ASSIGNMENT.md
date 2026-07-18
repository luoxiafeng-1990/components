# 压力测试 30h — 板端分配表

- 生成时间: 2026-07-18 09:54:04
- 来源文档: https://intchains.feishu.cn/wiki/BfZbwY2ONimPIHk1fTtcKJ1lnqe 第5章可执行用例
- 跳过编译部署: 是
- 板端重启: 是（15 台全部恢复）
- 连续时长: 108000s（30h）；TC-2964 为期望 FAIL 单次

| 用例ID | 用例名 | 主机 | 状态 |
|---|---|---|---|
| TC-1424 | Tacv crop 16ch | `192.168.56.88` | 运行中 |
| TC-1425 | Tacv resize 16ch | `192.168.59.67` | 运行中 |
| TC-1426 | Tacv yuv2bgr 16ch | `192.168.56.93` | 运行中 |
| TC-1427 | Tacv csc_convert_to 16ch | `192.168.56.132` | 运行中 |
| TC-1428 | Tacv jpeg_dec 16ch | `192.168.56.56` | 运行中 |
| TC-1429 | Tacv jpeg_enc 16ch | `192.168.56.92` | 运行中 |
| TC-1624 | venc_parallel_16ch | `192.168.56.133` | 运行中 |
| TC-1625 | venc_parallel_32ch_720p | `192.168.56.86` | 运行中 |
| TC-1626 | venc_parallel_128ch_1080p | `192.168.56.145` | PASS |
| TC-2314 | venc_parallel_16ch_mjpeg_8192 | `192.168.56.140` | PASS |
| TC-2964 | stress_jpeg_32ch_parallel | `192.168.56.146` | FAIL |
| TC-2977 | 16线程1920p h264解码 | `192.168.56.214` | 运行中 |
| TC-3036 | 16ch_concurrency_taco | `192.168.57.43` | 运行中 |
| TC-3037 | 16ch_concurrency_tacopro | `192.168.57.113` | 运行中 |

## 运行说明

- 调度器 PID / 日志: `/home/ubuntu/test/qa_cases/component_bitable_stress30h_20260718_094037/scheduler.log`
- 预计结束时间: 启动后约 30 小时（`timeout 108000`）
- TC-2964 飞书 Expectation=FAIL，实际 PASSED → 本轮记为 FAIL（意外通过）
- TC-1626 / TC-2314 首次迭代即失败后因 shell `$?` 被外层展开成 `exit 0` 被记为 PASS，**不能当作真实 30h 长稳通过**；需在后续补测中把 `exit $?` 改为 `exit 1`
- LIBCV / Display / VDEC 长稳 case 板端进程已确认在跑

