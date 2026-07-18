# 为何跑不满 30h / 日志丢失 — 根因与方案

## 根因（已核实）

1. **调度器误补扫（致命）**  
   `schedule_concurrent.py` 在队列被取空后约 10s 就 `break`，再 `join(timeout=5)`，把仍在 SSH 阻塞中的 30h case 当成「未执行」，启动补扫。  
   补扫在同机 `pkill -9 -f qa_cases` → **exit=137**，长稳被杀死。

2. **exit=-1 + 空日志**  
   SSH/worker 被截断后拿不到 `QA_DECODE_EXIT_CODE=`，本地记 -1；`/tmp` 日志未及时 SCP，表现为 0 字节。

3. **`$?` 被外层展开**  
   Procedure 里 `|| exit $?` 经 `bash -c "$PROC"` 变成 `|| exit 0`，VENC 失败一轮却假 PASS。

4. **VENC 本身不是时长型**  
   单次 `venc_parallel_*` 几秒～几分钟结束；必须外层 `while` + `timeout 108000` 才能墙钟 30h。

## 解决方案（已落地）

1. 修复调度器：队列空且仍有执行中时不得退出；worker/补扫按 `max(Timeout)+300` join。  
2. 新增 `soak_30h_runner.py`：板端 **nohup**，不挂 30h SSH；日志写 `/root/soak_logs/<TC>/soak.log`，默认 **每 5 分钟 SCP** 回本地。  
3. Procedure：禁止 `$?`；VENC 迭代失败记 `ITER_FAIL` 后继续，保证跑满 30h。  
4. `timeout` 到期 124/143 → PASS。
