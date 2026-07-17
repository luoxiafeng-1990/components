# QA_ENCODE 全功能测试重跑 — 启动状态

- **日期**: 2026-07-18
- **Test set**: QA_ENCODE（飞书拉取 426 条，可执行 381 条）
- **结果目录**: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_234050`
- **deb**: `workshop-qa-components_2.86+20260717234338_riscv64.deb`
- **主机**: 13 台空闲可达板（见 `hosts_ready.txt`）

## 测试前已完成

1. OpenVPN 连通 + `192.168.56.0/23` 走 tun0
2. Docker 重新编译 components 并打 deb
3. 每台板：purge `components` / `workshop-qa-components`，删除 `qa_cases` 与 `/opt/workshop-qa`，重装 `tps-test`
4. SCP 安装新 deb；缺失动态库从健康板 `192.168.56.132` 板间分发并 `ldconfig`
5. 调度器以 `--skip-compile-deploy --max-rounds 4` 启动（编译部署已手工完成）

## 调度进度（启动后抽样）

- 第 1 轮进行中：约 195/381 完成（13 台并发）
- 日志：`$COMPONENT_BITABLE_RUN_ROOT/scheduler.log`
- PID 文件：`$COMPONENT_BITABLE_RUN_ROOT/scheduler.pid`

完成后将按 skill 执行 P4.5 → P5 → P6 报告。
