"use client";

import React, { useState } from "react";

// ── 工具函数 ──────────────────────────────────────────────────────────
const cn = (...classes: (string | boolean | undefined | null)[]) =>
  classes.filter(Boolean).join(" ");

// ── 类型 ───────────────────────────────────────────────────────────────
interface TableRow {
  scenario: string;
  middleware: string;
  pattern: string;
  keyConsideration: string;
}

// ── 子组件 ─────────────────────────────────────────────────────────────

/** 带渐变光晕的节标题 */
const SectionTitle: React.FC<{
  number: string;
  title: string;
  subtitle?: string;
}> = ({ number, title, subtitle }) => (
  <div className="mb-6 flex items-center gap-4">
    <span className="flex h-10 w-10 shrink-0 items-center justify-center rounded-lg bg-gradient-to-br from-cyan-500 to-blue-600 text-sm font-bold text-white shadow-lg shadow-cyan-500/25">
      {number}
    </span>
    <div>
      <h2 className="text-2xl font-bold tracking-tight text-white">
        {title}
      </h2>
      {subtitle && (
        <p className="mt-0.5 text-sm text-slate-400">{subtitle}</p>
      )}
    </div>
  </div>
);

/** 卡片容器 */
const Card: React.FC<{
  children: React.ReactNode;
  className?: string;
}> = ({ children, className }) => (
  <div
    className={cn(
      "rounded-xl border border-slate-700/60 bg-slate-800/40 p-5 shadow-lg backdrop-blur-sm transition-all duration-200 hover:border-slate-600/80",
      className,
    )}
  >
    {children}
  </div>
);

/** 徽章标签 */
const Badge: React.FC<{
  children: React.ReactNode;
  variant?: "cyan" | "amber" | "emerald" | "rose" | "purple" | "slate";
}> = ({ children, variant = "slate" }) => {
  const map: Record<string, string> = {
    cyan: "bg-cyan-500/10 text-cyan-300 border-cyan-500/20",
    amber: "bg-amber-500/10 text-amber-300 border-amber-500/20",
    emerald: "bg-emerald-500/10 text-emerald-300 border-emerald-500/20",
    rose: "bg-rose-500/10 text-rose-300 border-rose-500/20",
    purple: "bg-purple-500/10 text-purple-300 border-purple-500/20",
    slate: "bg-slate-500/10 text-slate-300 border-slate-500/20",
  };
  return (
    <span
      className={cn(
        "inline-block rounded-full border px-3 py-0.5 text-xs font-medium",
        map[variant],
      )}
    >
      {children}
    </span>
  );
};

/** 条纹表格 */
const StyledTable: React.FC<{
  headers: string[];
  rows: TableRow[];
}> = ({ headers, rows }) => (
  <div className="overflow-x-auto rounded-lg">
    <table className="w-full text-left text-sm">
      <thead>
        <tr className="border-b border-slate-600/40 bg-slate-700/30">
          {headers.map((h, i) => (
            <th
              key={i}
              className="px-4 py-3 font-semibold text-slate-200 first:rounded-tl-lg last:rounded-tr-lg"
            >
              {h}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((row, i) => (
          <tr
            key={i}
            className={cn(
              "border-b border-slate-700/30 transition-colors hover:bg-slate-600/10",
              i % 2 === 0 ? "bg-slate-800/20" : "bg-transparent",
            )}
          >
            <td className="px-4 py-3 font-medium text-white">
              {row.scenario}
            </td>
            <td className="px-4 py-3 text-slate-300">{row.middleware}</td>
            <td className="px-4 py-3 text-slate-300">{row.pattern}</td>
            <td className="px-4 py-3 text-slate-400">{row.keyConsideration}</td>
          </tr>
        ))}
      </tbody>
    </table>
  </div>
);

/** 框架简介卡片（ROS 2 / CyberRT / Orocos / micro-ROS 共用） */
const FrameworkCard: React.FC<{
  icon: string;
  name: string;
  badge: string;
  badgeVariant?: "cyan" | "amber" | "emerald" | "rose" | "purple";
  description: string;
  children: React.ReactNode;
}> = ({ icon, name, badge, badgeVariant = "cyan", description, children }) => (
  <Card className="relative flex flex-col gap-4">
    {/* 顶部 */}
    <div className="flex items-start justify-between">
      <div className="flex items-center gap-3">
        <span className="text-3xl">{icon}</span>
        <div>
          <h3 className="text-lg font-bold text-white">{name}</h3>
          <Badge variant={badgeVariant}>{badge}</Badge>
        </div>
      </div>
    </div>
    <p className="text-sm leading-relaxed text-slate-300">{description}</p>
    <div className="space-y-2 text-sm text-slate-400">{children}</div>
  </Card>
);

/** 架构模式卡片 */
const PatternCard: React.FC<{
  icon: string;
  name: string;
  children: React.ReactNode;
}> = ({ icon, name, children }) => (
  <Card className="flex flex-col gap-3">
    <div className="flex items-center gap-3">
      <span className="text-2xl">{icon}</span>
      <h3 className="text-lg font-bold text-white">{name}</h3>
    </div>
    <div className="space-y-2 text-sm leading-relaxed text-slate-300">
      {children}
    </div>
  </Card>
);

// ── 数据 ───────────────────────────────────────────────────────────────

const recommendations: TableRow[] = [
  {
    scenario: "服务/导览机器人",
    middleware: "ROS 2",
    pattern: "行为树 + FSM 混合",
    keyConsideration: "丰富生态、快速原型",
  },
  {
    scenario: "工业机械臂",
    middleware: "Orocos + ROS 2",
    pattern: "分层控制（硬实时内环 + ROS 外环）",
    keyConsideration: "确定性 + 生态",
  },
  {
    scenario: "自动驾驶",
    middleware: "CyberRT",
    pattern: "分层架构 + 协程调度",
    keyConsideration: "高吞吐、确定性",
  },
  {
    scenario: "多机器人协同",
    middleware: "ROS 2 + DDS",
    pattern: "去中心化分布式",
    keyConsideration: "发现机制、QoS",
  },
  {
    scenario: "嵌入式/执行器",
    middleware: "micro-ROS",
    pattern: "反应式 / 包容架构",
    keyConsideration: "资源受限、实时性",
  },
  {
    scenario: "人形机器人",
    middleware: "ROS 2 + NVIDIA Isaac",
    pattern: "VLA 模型 + 分层控制",
    keyConsideration: "AI集成、仿真验证",
  },
];

// ── 主组件 ─────────────────────────────────────────────────────────────

const RoboticsSoftwareArchitectureCanvas: React.FC = () => {
  const [scrolled, setScrolled] = useState(false);

  React.useEffect(() => {
    const handler = () => setScrolled(window.scrollY > 40);
    window.addEventListener("scroll", handler, { passive: true });
    return () => window.removeEventListener("scroll", handler);
  }, []);

  return (
    <div className="min-h-screen bg-gradient-to-b from-slate-900 via-slate-900 to-slate-800 text-slate-200">
      {/* ── Hero ─────────────────────────────────────────────── */}
      <header className="relative overflow-hidden border-b border-slate-700/50">
        {/* 背景装饰 */}
        <div className="pointer-events-none absolute inset-0 bg-[radial-gradient(ellipse_at_top,_var(--tw-gradient-stops))] from-cyan-900/20 via-transparent to-transparent" />
        <div className="absolute -left-20 -top-20 h-64 w-64 rounded-full bg-cyan-500/5 blur-3xl" />
        <div className="absolute -right-20 -top-10 h-48 w-48 rounded-full bg-blue-500/5 blur-3xl" />

        <div className="relative mx-auto max-w-6xl px-6 pb-14 pt-20">
          <div className="flex items-center gap-3 text-sm font-medium text-cyan-400">
            <span className="h-px w-6 bg-cyan-400/60" />
            Research Report · 2026
          </div>
          <h1 className="mt-4 text-4xl font-extrabold leading-tight tracking-tight text-white md:text-5xl">
            机器人行业软件架构
            <br />
            <span className="bg-gradient-to-r from-cyan-300 to-blue-400 bg-clip-text text-transparent">
              调研报告
            </span>
          </h1>
          <p className="mt-4 max-w-2xl text-base leading-relaxed text-slate-400">
            深入剖析主流中间件框架、经典架构模式、通信中间件技术，以及
            VLA 基础模型与 AI 驱动架构等新兴趋势，为机器人系统选型提供全面参考。
          </p>
          <div className="mt-8 flex flex-wrap gap-3">
            <Badge variant="cyan">ROS 2</Badge>
            <Badge variant="amber">CyberRT</Badge>
            <Badge variant="emerald">Orocos</Badge>
            <Badge variant="purple">DDS / Zenoh</Badge>
            <Badge variant="rose">VLA 基础模型</Badge>
          </div>
        </div>
        {/* 进度条 */}
        <div
          className={cn(
            "fixed left-0 right-0 top-0 z-50 h-0.5 bg-gradient-to-r from-cyan-500 to-blue-600 transition-opacity duration-300",
            scrolled ? "opacity-100" : "opacity-0",
          )}
        />
      </header>

      {/* ── 主内容 ───────────────────────────────────────────── */}
      <main className="mx-auto max-w-6xl space-y-16 px-6 py-14">
        {/* ═══ 一、概览 ═══ */}
        <section>
          <SectionTitle number="01" title="概览" subtitle="Overview" />
          <Card>
            <p className="mb-4 leading-relaxed text-slate-300">
              机器人软件架构是机器人系统的骨架，决定了系统的可扩展性、实时性、安全性和可维护性。
              本次调研涵盖以下维度：
            </p>
            <div className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
              {[
                ["🧩", "主流中间件框架", "ROS 2、CyberRT、Orocos 等"],
                ["🏗️", "经典架构模式", "Sense-Plan-Act、分层控制、包容架构、行为树、状态机"],
                ["📡", "通信中间件技术", "DDS、Zenoh"],
                ["🚀", "新兴趋势", "VLA 基础模型、AI 驱动的架构"],
              ].map(([icon, title, desc]) => (
                <div
                  key={title}
                  className="rounded-lg border border-slate-700/40 bg-slate-800/30 p-4 transition-colors hover:border-slate-600/60"
                >
                  <div className="text-2xl">{icon}</div>
                  <div className="mt-2 text-sm font-semibold text-white">
                    {title}
                  </div>
                  <div className="mt-1 text-xs text-slate-400">{desc}</div>
                </div>
              ))}
            </div>
          </Card>
        </section>

        {/* ═══ 二、主流中间件 / 框架 ═══ */}
        <section>
          <SectionTitle
            number="02"
            title="主流中间件 / 框架"
            subtitle="Middleware &amp; Frameworks"
          />
          <div className="grid gap-6 md:grid-cols-2">
            {/* ROS 2 */}
            <FrameworkCard
              icon="🤖"
              name="ROS 2 (Robot Operating System 2)"
              badge="行业事实标准"
              description="非传统 OS，而是构建于 Linux 等系统之上的分布式通信框架。2026 Lyrical Luth 版本发布，Zenoh 成为 Tier 1 中间件选项。"
            >
              <div>
                <span className="font-semibold text-cyan-300">分层架构：</span>
                OS 层 → DDS 中间件层 → RMW 抽象层 → 客户端库层 (rclcpp/rclpy)
                → 应用层
              </div>
              <div>
                <span className="font-semibold text-cyan-300">通信机制：</span>
                基于 DDS，支持发布/订阅、请求/响应、Actions
              </div>
              <div>
                <span className="font-semibold text-cyan-300">核心特性：</span>
                去中心化发现、丰富 QoS 策略、生命周期管理、节点组件化
              </div>
              <div>
                <span className="font-semibold text-slate-300">适用场景：</span>
                复杂多组件系统、需要丰富生态的机器人项目
              </div>
            </FrameworkCard>

            {/* CyberRT */}
            <FrameworkCard
              icon="⚡"
              name="Apollo CyberRT"
              badge="百度自动驾驶"
              badgeVariant="amber"
              description="百度 Apollo 自动驾驶平台的高性能运行时框架，基于协程的用户态调度设计，大幅减少线程切换开销。"
            >
              <div>
                <span className="font-semibold text-amber-300">分层架构：</span>
                应用层 → 运行时层 (调度器 + 消息传输) → 基础设施层 (服务发现 +
                资源管理)
              </div>
              <div>
                <span className="font-semibold text-amber-300">调度策略：</span>
                Classic (经典优先级) 和 Choreography (编排) 两种策略
              </div>
              <div>
                <span className="font-semibold text-amber-300">通信：</span>
                支持共享内存 (SHM) 和 RTPS 协议，低延迟高吞吐
              </div>
              <div>
                <span className="font-semibold text-slate-300">适用场景：</span>
                自动驾驶等对实时性要求极高的系统
              </div>
            </FrameworkCard>

            {/* Orocos */}
            <FrameworkCard
              icon="🔧"
              name="Orocos"
              badge="硬实时控制"
              badgeVariant="emerald"
              description="面向硬实时控制环路的开源机器人控制软件，微秒级确定性执行，适合 IEC 61508 功能安全场景。"
            >
              <div>
                <span className="font-semibold text-emerald-300">核心优势：</span>
                微秒级确定性执行
              </div>
              <div>
                <span className="font-semibold text-slate-300">适用场景：</span>
                工业机器人伺服控制、安全关键系统
              </div>
            </FrameworkCard>

            {/* micro-ROS */}
            <FrameworkCard
              icon="📟"
              name="micro-ROS"
              badge="嵌入式"
              badgeVariant="rose"
              description="ROS 2 的嵌入式微控制器版本，可在 RTOS 上运行，适用于资源受限的嵌入式设备和执行器端。"
            >
              <div>
                <span className="font-semibold text-slate-300">典型硬件：</span>
                STM32、ESP32 等 MCU
              </div>
            </FrameworkCard>

            {/* 其他框架 ─ 占两列 */}
            <div className="md:col-span-2">
              <Card>
                <h3 className="mb-3 flex items-center gap-2 text-base font-bold text-white">
                  <span className="text-xl">📦</span> 其他值得关注的框架
                </h3>
                <div className="grid gap-4 sm:grid-cols-3">
                  <div>
                    <div className="text-sm font-semibold text-cyan-300">
                      YARP
                    </div>
                    <p className="mt-1 text-xs text-slate-400">
                      Yet Another Robot Platform，用于人形机器人分布式系统
                    </p>
                  </div>
                  <div>
                    <div className="text-sm font-semibold text-purple-300">
                      NVIDIA Isaac
                    </div>
                    <p className="mt-1 text-xs text-slate-400">
                      Isaac Sim (仿真)、Isaac Lab (机器人学习)、Jetson Thor
                      (边缘计算)
                    </p>
                  </div>
                  <div>
                    <div className="text-sm font-semibold text-emerald-300">
                      LeRobot (Hugging Face)
                    </div>
                    <p className="mt-1 text-xs text-slate-400">
                      基于模仿学习和强化学习的开源机器人策略训练库
                    </p>
                  </div>
                </div>
              </Card>
            </div>
          </div>
        </section>

        {/* ═══ 三、经典架构模式 ═══ */}
        <section>
          <SectionTitle
            number="03"
            title="经典架构模式"
            subtitle="Architecture Patterns"
          />
          <div className="grid gap-6 md:grid-cols-2">
            <PatternCard icon="🔄" name="Sense-Plan-Act（感知-规划-执行）">
              <ul className="list-inside list-disc space-y-1 text-slate-400">
                <li>
                  经典三阶段流水线：传感器处理 → 环境理解与规划 → 执行控制
                </li>
                <li>
                  <span className="text-emerald-400">优点：</span>
                  结构清晰、易于调试
                </li>
                <li>
                  <span className="text-rose-400">缺点：</span>
                  线性流程，感知耗时可能拖慢整体响应
                </li>
                <li>适用：慢速移动机器人</li>
              </ul>
            </PatternCard>

            <PatternCard icon="📊" name="分层控制架构 (Layered Control)">
              <p>典型分层（NIST 4D/RCS 参考模型）：</p>
              <ul className="mt-1 list-inside list-disc space-y-0.5 text-slate-400">
                <li>
                  <span className="text-white">Mission</span> —— 任务规划（分钟级）
                </li>
                <li>
                  <span className="text-white">Task Planning</span> ——
                  任务调度（秒-分钟级）
                </li>
                <li>
                  <span className="text-white">Behavioral</span> ——
                  行为协调（0.1-10 秒级）
                </li>
                <li>
                  <span className="text-white">Motion Planning</span> ——
                  轨迹规划（10-100 毫秒级）
                </li>
                <li>
                  <span className="text-white">Reactive / Skill</span> ——
                  反应式技能（1-100 毫秒级）
                </li>
                <li>
                  <span className="text-white">Servo</span> ——
                  伺服控制（0.1-10 毫秒级）
                </li>
              </ul>
              <p className="mt-2 text-xs text-slate-500">
                现代变体在层之间增加了侧向连接以处理异常
              </p>
            </PatternCard>

            <PatternCard icon="🧱" name="包容架构 (Subsumption)">
              <ul className="list-inside list-disc space-y-1 text-slate-400">
                <li>Rodney Brooks 1986 年提出，基于行为的反应式架构</li>
                <li>无中心控制模型，行为按层级组织</li>
                <li>
                  高层行为可以{" "}
                  <span className="font-medium text-white">"包容"</span>
                  （抑制/取代）低层行为
                </li>
                <li>
                  <span className="text-emerald-400">优势：</span>
                  实时响应动态环境
                </li>
                <li>
                  <span className="text-rose-400">局限：</span>
                  难以学习复杂动作、深层建图和语言理解
                </li>
              </ul>
            </PatternCard>

            <PatternCard icon="🌳" name="行为树 (Behavior Tree)">
              <ul className="list-inside list-disc space-y-1 text-slate-400">
                <li>
                  从游戏 AI 引入机器人领域，近年成为 ROS 2 官方推荐方案
                </li>
                <li>
                  树形结构：根节点 → 控制节点 (Sequence/Selector/Parallel/Decorator)
                  → 执行节点 (Action/Condition)
                </li>
                <li>三种返回状态：Success、Failure、Running</li>
                <li>
                  <span className="text-emerald-400">相比 FSM 优势：</span>
                  模块化程度高、易于组合和修改
                </li>
                <li>
                  推荐库：
                  <code className="ml-1 rounded bg-slate-700/60 px-1.5 py-0.5 font-mono text-xs text-cyan-300">
                    BehaviorTree.CPP
                  </code>
                  、
                  <code className="ml-1 rounded bg-slate-700/60 px-1.5 py-0.5 font-mono text-xs text-cyan-300">
                    py_trees
                  </code>
                </li>
              </ul>
            </PatternCard>

            <div className="md:col-span-2">
              <PatternCard icon="⚙️" name="有限状态机 (FSM)">
                <ul className="list-inside list-disc space-y-1 text-slate-400">
                  <li>
                    经典的任务调度方式，ROS 1 时代常用
                    <code className="ml-1 rounded bg-slate-700/60 px-1.5 py-0.5 font-mono text-xs text-cyan-300">
                      SMACH
                    </code>
                  </li>
                  <li>
                    适用：高层模式管理（巡逻 / 警戒 / 充电等系统状态切换）
                  </li>
                  <li>
                    <span className="font-medium text-white">
                      混合架构趋势：
                    </span>
                    FSM 做 "模式管理" + BT 做 "行为执行"
                  </li>
                </ul>
              </PatternCard>
            </div>
          </div>
        </section>

        {/* ═══ 四、通信中间件技术 ═══ */}
        <section>
          <SectionTitle
            number="04"
            title="通信中间件技术"
            subtitle="Communication Middleware"
          />
          <div className="grid gap-6 md:grid-cols-2">
            <Card>
              <div className="mb-3 flex items-center gap-3">
                <span className="text-2xl">📡</span>
                <h3 className="text-lg font-bold text-white">
                  DDS (Data Distribution Service)
                </h3>
              </div>
              <ul className="space-y-2 text-sm leading-relaxed text-slate-300">
                <li>OMG 标准，ROS 2 的默认通信层</li>
                <li>去中心化的数据分发服务，支持发布/订阅</li>
                <li>
                  丰富的 QoS 策略：可靠性 (RELIABLE/BEST_EFFORT)、持久性、截止时间、活跃度
                </li>
                <li>
                  主流实现：
                  <code className="ml-1 rounded bg-slate-700/60 px-1.5 py-0.5 font-mono text-xs">
                    Fast DDS
                  </code>
                  、
                  <code className="ml-1 rounded bg-slate-700/60 px-1.5 py-0.5 font-mono text-xs">
                    Cyclone DDS
                  </code>
                  、
                  <code className="ml-1 rounded bg-slate-700/60 px-1.5 py-0.5 font-mono text-xs">
                    Connext DDS
                  </code>
                </li>
                <li className="text-rose-400">
                  ⚠ 局限：配置复杂，10-20 节点以上需域分区
                </li>
              </ul>
            </Card>

            <Card>
              <div className="mb-3 flex items-center gap-3">
                <span className="text-2xl">⚡</span>
                <h3 className="text-lg font-bold text-white">Zenoh（2026 新兴方案）</h3>
              </div>
              <ul className="space-y-2 text-sm leading-relaxed text-slate-300">
                <li>Eclipse 基金会的轻量级发布/订阅协议</li>
                <li>
                  ROS 2 Lyrical Luth 版本中成为 Tier 1
                  中间件，计划未来成为默认通信方案
                </li>
                <li className="text-emerald-400">
                  延迟、吞吐量和内存占用优于传统 DDS 方案
                </li>
                <li>特别适合云边端协同场景</li>
              </ul>
            </Card>
          </div>
        </section>

        {/* ═══ 五、实时性与安全关键设计 ═══ */}
        <section>
          <SectionTitle
            number="05"
            title="实时性与安全关键设计"
            subtitle="Real-Time &amp; Safety-Critical"
          />
          <div className="grid gap-6 md:grid-cols-2">
            <Card>
              <h3 className="mb-3 flex items-center gap-2 text-base font-bold text-white">
                <span>⏱️</span> 实时性分区
              </h3>
              <ul className="space-y-3 text-sm leading-relaxed text-slate-300">
                <li className="flex items-start gap-2">
                  <span className="mt-0.5 h-2 w-2 shrink-0 rounded-full bg-rose-500" />
                  <div>
                    <span className="font-semibold text-rose-300">
                      硬实时任务
                    </span>
                    （伺服控制、安全监控）
                    <br />
                    → RTOS (VxWorks / QNX / PREEMPT_RT Linux)
                  </div>
                </li>
                <li className="flex items-start gap-2">
                  <span className="mt-0.5 h-2 w-2 shrink-0 rounded-full bg-amber-500" />
                  <div>
                    <span className="font-semibold text-amber-300">
                      软实时任务
                    </span>
                    （路径规划、感知推理）
                    <br />
                    → 通用 OS (Ubuntu 等)
                  </div>
                </li>
                <li className="mt-2 rounded-lg border border-rose-500/20 bg-rose-500/5 p-3 text-xs text-rose-300">
                  ⚠ 混搭设计是工业机器人故障的已知来源
                </li>
              </ul>
            </Card>

            <Card>
              <h3 className="mb-3 flex items-center gap-2 text-base font-bold text-white">
                <span>🔒</span> 安全认证
              </h3>
              <ul className="space-y-2 text-sm leading-relaxed text-slate-300">
                <li>
                  <span className="font-semibold text-white">IEC 61508</span>{" "}
                  （功能安全）、
                  <span className="font-semibold text-white">ISO 10218</span>{" "}
                  （工业机器人安全）
                </li>
                <li>
                  ROS 2 本身
                  <span className="font-medium text-rose-400">
                    无安全认证
                  </span>
                  ，需要结合认证 RTOS
                </li>
                <li>
                  安全逻辑必须与应用程序在架构上
                  <span className="font-medium text-amber-300">
                    独立分离
                  </span>
                </li>
              </ul>
            </Card>
          </div>
        </section>

        {/* ═══ 六、新兴趋势 ═══ */}
        <section>
          <SectionTitle
            number="06"
            title="新兴趋势（2025-2026）"
            subtitle="Emerging Trends"
          />
          <div className="grid gap-6 md:grid-cols-3">
            <Card>
              <div className="mb-3 flex items-center gap-2">
                <span className="text-2xl">🧠</span>
                <h3 className="text-base font-bold text-white">
                  VLA 基础模型
                </h3>
              </div>
              <ul className="space-y-2 text-sm leading-relaxed text-slate-300">
                <li>Vision-Language-Action 模型：RT-2-X、π0、GPT-4V Robotics、Gemini Robotics</li>
                <li>
                  自然语言直接指挥机器人：用
                  <span className="italic text-cyan-300">
                    "把蓝色工具放到工作台上"
                  </span>
                  替代手写代码
                </li>
                <li>消除任务特定编程，实现零样本或少样本适应</li>
              </ul>
            </Card>

            <Card>
              <div className="mb-3 flex items-center gap-2">
                <span className="text-2xl">🤖</span>
                <h3 className="text-base font-bold text-white">
                  AI 驱动的架构演进
                </h3>
              </div>
              <ul className="space-y-2 text-sm leading-relaxed text-slate-300">
                <li>仿真到现实迁移（Sim-to-Real）：NVIDIA Isaac Sim + 数字孪生</li>
                <li>AI 辅助监控和调优</li>
                <li>边缘计算集成，降低决策延迟</li>
              </ul>
            </Card>

            <Card>
              <div className="mb-3 flex items-center gap-2">
                <span className="text-2xl">🌐</span>
                <h3 className="text-base font-bold text-white">
                  开源生态扩张
                </h3>
              </div>
              <ul className="space-y-2 text-sm leading-relaxed text-slate-300">
                <li>
                  2026 年 ROS 2 占据约
                  <span className="font-semibold text-white">58%</span>
                  的 ROS 下载量
                </li>
                <li>
                  LeRobot 等开源库降低机器人学习门槛
                </li>
                <li>多家框架融合趋势明显</li>
              </ul>
            </Card>
          </div>
        </section>

        {/* ═══ 七、选型建议表 ═══ */}
        <section>
          <SectionTitle
            number="07"
            title="选型建议表"
            subtitle="Recommendation Matrix"
          />
          <Card className="overflow-hidden !p-0">
            <StyledTable
              headers={["场景", "推荐中间件", "推荐架构模式", "关键考量"]}
              rows={recommendations}
            />
          </Card>
        </section>

        {/* ═══ 八、总结 ═══ */}
        <section>
          <SectionTitle number="08" title="总结" subtitle="Conclusion" />
          <Card>
            <div className="grid gap-4 sm:grid-cols-2">
              {[
                [
                  "🧩",
                  "ROS 2 仍是主流基石",
                  "正从"是否能用"转向"能否规模化"",
                ],
                [
                  "🔗",
                  "混合架构是行业共识",
                  "没有单一架构解决所有问题",
                ],
                [
                  "🚀",
                  "实时性与 AI 大模型的结合",
                  "是当前最大趋势",
                ],
                [
                  "📡",
                  "Zenoh 可能重塑",
                  "机器人通信格局",
                ],
              ].map(([icon, a, b]) => (
                <div
                  key={a}
                  className="flex items-start gap-3 rounded-lg border border-slate-700/40 bg-slate-800/30 p-4"
                >
                  <span className="text-xl">{icon}</span>
                  <div>
                    <div className="text-sm font-semibold text-white">{a}</div>
                    <div className="mt-0.5 text-xs text-slate-400">{b}</div>
                  </div>
                </div>
              ))}
            </div>
            <div className="mt-6 border-t border-slate-700/40 pt-5">
              <p className="text-center text-sm text-slate-400">
                选型需综合考虑：实时性要求、安全认证、生态依赖、团队技能
              </p>
            </div>
          </Card>
        </section>
      </main>

      {/* ── 页脚 ─────────────────────────────────────────────── */}
      <footer className="border-t border-slate-700/40">
        <div className="mx-auto max-w-6xl px-6 py-8 text-center text-xs text-slate-600">
          <p>机器人行业软件架构调研报告 · 2026</p>
          <p className="mt-1">Generated with Cursor Canvas</p>
        </div>
      </footer>
    </div>
  );
};

export default RoboticsSoftwareArchitectureCanvas;
