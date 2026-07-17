# LOG_REVIEW_REPORT — QA_DECODE 严格 P5 复查

- 生成时间: 2026-07-17 09:48:40
- 结果根目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260716`
- 复查总数: **126**（= 本轮执行 case 总数）
- 一致数: **126**
- 不一致数: **0**
- FINAL 统计: PASS=106, FAIL=20

## 1. 一致性结论

全量复查未发现 result.txt 与日志/Expectation 判定不一致；FAIL 条目均已写入三段式与 FAIL_REASON。

## 2. FAIL 归因统计（P5.2/P5.3 严格 cat 后）

| FAIL_REASON | 数量 | TC 列表 |
|-------------|------|---------|
| host_env_error | 7 | TC-1456, TC-1459, TC-1460, TC-2317, TC-2319, TC-2321, TC-2322 |
| resource_missing | 5 | TC-1537, TC-1538, TC-1545, TC-1546, TC-1547 |
| hardware_limit | 4 | TC-1560, TC-1562, TC-1574, TC-1575 |
| case_fail | 3 | TC-1485, TC-1580, TC-1583 |
| procedure_error | 1 | TC-2977 |

## 3. FAIL 明细

| TC-ID | exit | FAIL_REASON | ROOT_CAUSE | Host |
|-------|------|-------------|------------|------|
| TC-1456 | -1 | host_env_error | rtsp_compare_incomplete_timeout | 192.168.57.113 |
| TC-1459 | -1 | host_env_error | rtsp_codec_mismatch_hevc_vs_h264 | 192.168.56.92 |
| TC-1460 | -1 | host_env_error | rtsp_codec_mismatch_hevc_vs_h264 | 192.168.56.137 |
| TC-1485 | 139 | case_fail | hevc_hw_timeout_frame_error | 192.168.56.145 |
| TC-1537 | 1 | resource_missing | missing_48x48_jpg | 192.168.56.145 |
| TC-1538 | 1 | resource_missing | missing_48x48_jpg | 192.168.57.113 |
| TC-1545 | 1 | resource_missing | missing_48x48_jpg | 192.168.56.145 |
| TC-1546 | 1 | resource_missing | missing_48x48_jpg | 192.168.56.145 |
| TC-1547 | 1 | resource_missing | missing_48x48_jpg | 192.168.57.113 |
| TC-1560 | -1 | hardware_limit | resolution_exceeds_8192 | 192.168.57.113 |
| TC-1562 | -1 | hardware_limit | resolution_exceeds_8192 | 192.168.56.145 |
| TC-1574 | -1 | hardware_limit | resolution_exceeds_8192 | 192.168.56.137 |
| TC-1575 | -1 | hardware_limit | pp_upscale_unsupported | 192.168.56.92 |
| TC-1580 | -1 | case_fail | multi_worker_compare_hang_timeout | 192.168.56.145 |
| TC-1583 | -1 | case_fail | multi_worker_compare_hang_timeout | 192.168.57.113 |
| TC-2317 | -1 | host_env_error | rtsp_codec_mismatch_hevc_vs_h264 | 192.168.56.92 |
| TC-2319 | -1 | host_env_error | rtsp_codec_mismatch_hevc_vs_h264 | 192.168.56.137 |
| TC-2321 | -1 | host_env_error | rtsp_codec_mismatch_hevc_vs_h264 | 192.168.56.145 |
| TC-2322 | -1 | host_env_error | rtsp_codec_mismatch_hevc_vs_h264 | 192.168.57.113 |
| TC-2977 | -1 | procedure_error | infinite_loop_timeout_too_short | 192.168.56.137 |

## 4. Flaky（retested_pass）

集合: TC-2320

### TC-2320 双套对比（P5.2bis）

| 维度 | 首次尝试（未落地完整 log） | 后续成功运行 |
|------|---------------------------|--------------|
| 时间/主机 | 18:16:18 @ 192.168.56.145 | 18:26:11 @ 192.168.56.92（补扫） |
| 结果产物 | 无 `result.txt`/`qa_cases_full.log` 首轮副本（被记为未执行→post_sweep） | EXIT=0，Status/Result=PASSED，Compare 287 frames PSNR=100 |
| RTSP | 摄像头 .225 已 dead，回退轮询池；首轮未完成 | `rtsp://...119:554/profile1`，codec=H264 1920x1080 匹配 |
| 物理归因 | 首轮分配后未完成（补扫前被计未执行；同期补扫出现 exit=137 强杀痕迹） | 换主机+稳定 H264 RTSP，硬软解同步比较通过 |
| 状态反转机制 | post_sweep 将未执行 case 重新分配到健康主机 | 成功侧完整初始化→300 packets→Compare PASSED |

## 5. P5.2 阅读确认

以下 20 条 FAIL 均已通过 `cat` 读取 `qa_cases_full.log`（小日志全文；大日志 head250+tail500）并对照 `result.txt`：

TC-1456, TC-1459, TC-1460, TC-1485, TC-1537, TC-1538, TC-1545, TC-1546, TC-1547, TC-1560, TC-1562, TC-1574, TC-1575, TC-1580, TC-1583, TC-2317, TC-2319, TC-2321, TC-2322, TC-2977

=== P5.4 完成：LOG_REVIEW_REPORT.md 已生成 ===
