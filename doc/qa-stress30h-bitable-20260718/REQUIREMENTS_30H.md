# 压力测试 30h 需求固化（用户 2026-07-18）

1. **必须跑满 30 小时**：每个 case 墙钟 ≥ 108000s。提前退出视为方案问题 → runner 自动重拉；短 Timeout/无 while 的 Procedure（如原 TC-2964）强制包装为 `timeout 108000 + while true`。
2. **必须收集 log + 板子映射**：`soak/<TC>/soak.log`、`HOST.txt`、`SOAK_ASSIGNMENT.md` / `HOST_CASE_LOG_MAP.md`。
3. **每 10 分钟表格统计**：`STATUS.md`（及 `STATUS_HISTORY.md`、`.session_bridge/SOAK_STATUS.md`），字段含 CASE/IP/状态/已跑/目标/剩余/日志字节/重启次数。

本轮目录：`/home/ubuntu/test/qa_cases/component_bitable_stress30h_rerun_20260718_162012`
