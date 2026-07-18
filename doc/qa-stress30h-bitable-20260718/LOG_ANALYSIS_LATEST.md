# 30h Soak LOG 分析（2026-07-18 21:19:40）
- wrapper RUN: **13/14**
- 健康: **8** / 有错误但持续: **2** / 异常偏高: **4**

| CASE | IP | 状态 | 已跑 | 日志 | ITER_FAIL | 次/时 | ERROR类 | qa/tacv | 健康 | 最近关键日志 |
|---|---|---|---|---:|---:|---:|---:|---|---|---|
| TC-1424 | `192.168.56.140` | RUN | 04:58:47 | 1.2KB | 0 | 0.0 | 0 | 0/1 | **健康** | PASS |
| TC-1425 | `192.168.56.145` | RUN | 04:58:45 | 96B | 0 | 0.0 | 0 | 0/1 | **健康** | PASS |
| TC-1426 | `192.168.56.214` | RUN | 04:58:42 | 34.1KB | 0 | 0.0 | 0 | 0/1 | **健康** | total_size_in 3112960 , total_size_out 6221824 |
| TC-1427 | `192.168.56.39` | RUN | 04:58:40 | 65.9KB | 0 | 0.0 | 0 | 0/1 | **健康** | total_size_in 3112960 , total_size_out 6221824 |
| TC-1428 | `192.168.56.56` | RUN | 04:58:38 | 14.2MB | 0 | 0.0 | 292381 | 0/1 | **有错误但持续** | [ERROR] CpuSyncEnd() failure: Bad file descriptor |
| TC-1429 | `192.168.56.86` | RUN | 04:58:34 | 6.8KB | 0 | 0.0 | 14 | 0/1 | **健康** | 2026-07-18 21:14:57 ERROR [3399] ta_cv_jpeg_enc_one_image() 171:[Tacv] JPEG HW Encode Failed! |
| TC-1624 | `192.168.56.93` | DEAD |  | 0B | 0 | 0.0 | 0 | 0/0 | **wrapper死** |  |
| TC-1625 | `192.168.57.113` | RUN | 04:58:46 | 2471.1MB | 33 | 6.6 | 5388002 | 1/0 | **有错误但持续** | [2026-07-18 21:19:19.540] [components.VideoProductionLine] [ERROR] [Thread #0] fillBuffer terminal error: [Codec] Encode |
| TC-1626 | `192.168.57.43` | RUN | 04:58:47 | 2403.8MB | 2481 | 498.2 | 4975633 | 1/0 | **异常偏高** | ITER_FAIL:2026-07-18_16:20:53 |
| TC-2314 | `192.168.56.88` | RUN | 04:58:28 | 483.0MB | 8388 | 1686.2 | 469784 | 0/0 | **异常偏高** | ITER_FAIL:2026-07-18_16:20:55 |
| TC-2964 | `192.168.56.146` | RUN | 04:58:24 | 102.7MB | 3451 | 693.9 | 107012 | 0/0 | **异常偏高** | [2026-07-18 21:19:17.641] [components.VideoProductionLine] [ERROR] [Thread #0] fillBuffer terminal error: [Codec] Encode |
| TC-2977 | `192.168.56.92` | RUN | 04:58:33 | 25.5MB | 0 | 0.0 | 0 | 1/0 | **健康** | [2026-07-18 21:19:14.446] [components.Worker.Decode] [DEBUG] Successfully seeked to frame 0 |
| TC-3036 | `192.168.56.132` | RUN | 04:58:54 | 127.1MB | 0 | 0.0 | 54 | 1/0 | **健康** | [2026-07-18 21:19:18.034] [components.Worker.Decode] [DEBUG] Successfully seeked to frame 0 |
| TC-3037 | `192.168.56.133` | RUN | 04:59:01 | 1757.2MB | 0 | 0.0 | 0 | 1/0 | **健康** | [2026-07-18 21:19:17.992] [components.TacoProStitcherDriver] [DEBUG] stitch: creating output image: 1280x720 fmt=NV12 bl |

## 判定说明
- **健康**：wrapper 存活，ITER_FAIL 低，无持续致命错误
- **有错误但持续**：仍在跑，但 log 中 ERROR/编码失败较多（压力场景常见）
- **异常偏高**：ITER_FAIL/小时 > 30，说明单次迭代失败很频繁，需重点看
- **间歇/空档**：采样时无业务进程（while 循环的 sleep 间隙），wrapper 仍可能 RUN

