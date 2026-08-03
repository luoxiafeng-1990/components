# 架构文档 Review（2026-08-03）

## 总评

| 维度 | 结论 |
|------|------|
| MultiWorker / BufferPool 内核 | **准确**，`ARCHITECTURE.md` 可作权威 |
| 插件 → ExecuteMode 全链路 | **缺口大**，缺 Wiki 级总图（本 Wiki 补齐） |
| CLI 测试框架图 | **过时风险高**（早于 CLI11 多子命令一视同仁模型） |
| WebUI 预览规格 | **准确且已认可**，但 Phase1 板端验收未完成 |
| 根 README | **空模板**，对新人几乎无帮助 |

## 逐份评审

| 文档 | 结论 | 建议动作 | 优先级 |
|------|------|----------|--------|
| `ARCHITECTURE.md` | 内核准；几乎不提 `IOptionPlugin` / `qa_cases` | 增补一章「插件与 ExecuteMode」并链到本 Wiki | P1 |
| `webui/docs/PREVIEW_ARCHITECTURE_DESIGN.md` | 规格准确（方案 3 已认可） | 文首标注实施状态：Phase1 待板端验收 | P2 |
| `webui/docs/API.md` | API↔WorkerConfig 映射准确 | 与预览解耦规格对齐 JPEG / FrameTap 语义 | P2 |
| `webui/README.md` | 分层摘要清楚 | 保留；链到本 Wiki Home | P3 |
| `doc/test_framework_architecture.drawio` | 过时风险高 | 按 CLI11 多子命令 + 驱动/伴随模型重绘或归档 | P1 |
| `doc/worker_architecture.drawio` + `*_html` | 基本准、偏旧（约 6 月） | 补 FrameTap、per-config PARALLEL flags | P2 |
| `MultiWorkerProductionLine_Architecture.*` | 与内核文档配套，准确 | 保留；在 Home 互链 | P3 |
| `FRAME_SYNC_EXAMPLE.md` | COMPARE / sync 用法准确 | 保留作示例 | P3 |
| `doc/consumptionline_BufferComparator.md` | 概念对；include 路径过时 | 更正路径到 `consumptionline/types/compare/` | P2 |
| `README.md`（根） | GitLab 模板空壳 | 改为指向 Wiki + 三条常用命令 | P1 |
| `docs/superpowers/plans/2026-07-19-webui-preview-architecture.md` | 任务文件引用但仓库内未找到 | 补回计划文件或删无效引用 | P2 |
| `doc/display/*`、`doc/npu-tutorial/*`、`doc/linux-drivers/*` | 子系统准确 | 保持专题；不充当总架构 | — |
| `doc/qa-*-bitable-*` | 运行报告 | 不作为架构权威 | — |

## 已由本 Wiki 补齐的缺口

1. **分层阅读结构**（Home：应用 → 契约 → 执行 → 运行时）
2. **`test_module_main.cpp` 应用案例**（Application-Case-qa_cases-main）
3. **`IOptionPlugin` 逐方法契约**（IOptionPlugin-Interface）
4. **驱动 / 伴随 / 工具角色与注册表**（Plugin-Framework）
5. **ExecuteMode 决策树**（ExecuteMode-Routing）
6. **COMPARE PEER vs SOURCE_REF**（COMPARE-Paths）
7. **WebUI 复用同一框架**（Production-Consumption）
8. **文档可信度表**（本页）

## 建议后续（仓库内）

1. 根 `README.md` 增加 Wiki 链接与最小命令示例  
2. `ARCHITECTURE.md` 顶部增加「上层入口见 Wiki: Plugin-Framework」  
3. 归档或重绘 `doc/test_framework_architecture.drawio`  
4. 修正 `BufferComparator` 文档中的 include 路径  

> 本 Review 基于 2026-08-03 源码与文档对照；不改动运行时行为。
