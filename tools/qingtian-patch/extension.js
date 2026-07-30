"use strict";
/**
 * QingTian clean entry (replaces obfuscated extension.js).
 * Local trial / USDT billing — no activation-code gate.
 *
 * Critical: readable modules need real node_modules (esp. `ws`).
 * Register the sidebar webview FIRST so a later optional failure
 * cannot leave users with a completely blank panel.
 */
const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
const os = require("os");
const cp = require("child_process");
const crypto = require("crypto");

let _context = null;
let _provider = null;
let _updateNotice = null;
let _batchRetryEngine = null;
let _bridgeChild = null;
let _statusTimer = null;
let _activation = null;
let _mcpServer = null;
let _statusPayload = null;

function getExtensionContext() {
  return _context;
}

function safeRequire(rel) {
  try {
    return require(rel);
  } catch (e) {
    console.error(`[QingTian] require failed: ${rel}`, e);
    return null;
  }
}

function getExtensionPath() {
  return _context?.extensionPath || path.join(__dirname, "..");
}

function getVersion() {
  try {
    return require("../package.json").version;
  } catch {
    return "0.0.0";
  }
}

function resolveNodeCommand() {
  const candidates = ["/usr/bin/node", "/usr/local/bin/node", "/opt/homebrew/bin/node", "node"];
  for (const c of candidates) {
    try {
      if (c === "node") return c;
      if (fs.existsSync(c)) return c;
    } catch {}
  }
  return process.execPath || "node";
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function readJsonSafe(file, fallback) {
  try {
    if (!fs.existsSync(file)) return fallback;
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch {
    return fallback;
  }
}

function writeJsonAtomic(file, data) {
  ensureDir(path.dirname(file));
  const tmp = file + ".tmp";
  fs.writeFileSync(tmp, JSON.stringify(data, null, 2), "utf8");
  fs.renameSync(tmp, file);
}

/** Avoid rewriting identical mcp.json — Cursor restarts MCP stdio clients on file change. */
function writeJsonAtomicIfChanged(file, data) {
  try {
    if (fs.existsSync(file)) {
      const cur = JSON.parse(fs.readFileSync(file, "utf8"));
      if (JSON.stringify(cur) === JSON.stringify(data)) {
        return false;
      }
    }
  } catch {}
  writeJsonAtomic(file, data);
  return true;
}

function getWorkspaceMcpConfigFile() {
  const folders = vscode.workspace.workspaceFolders;
  if (!folders?.length) return null;
  return path.join(folders[0].uri.fsPath, ".cursor", "mcp.json");
}

function stripQingTianMcpServers(mcpServers) {
  const next = { ...(mcpServers || {}) };
  for (const key of Object.keys(next)) {
    if (/^qtwx-mcp-\d+$/.test(key)) delete next[key];
  }
  return next;
}

function getMcpRuntimeStampLocal(versionFallback) {
  try {
    if (_statusPayload?.getMcpRuntimeStamp) {
      return _statusPayload.getMcpRuntimeStamp(versionFallback);
    }
    const script = _mcpServer?.getMCPServerPath?.();
    if (script && fs.existsSync(script)) {
      return crypto.createHash("sha1").update(fs.readFileSync(script)).digest("hex").slice(0, 12);
    }
  } catch {}
  return versionFallback;
}

function buildQingTianMcpServers(channelCount, template) {
  const scriptPath = String(
    template?.args?.[0] || _mcpServer.getMCPServerPath()
  ).replace(/\\/g, "/");
  const queueRoot = String(
    template?.env?.QINGTIAN_QUEUE_ROOT || _mcpServer.getQueueRoot()
  ).replace(/\\/g, "/");
  const stamp = String(
    template?.env?.QINGTIAN_RUNTIME_STAMP || getMcpRuntimeStampLocal(getVersion())
  );
  const nodeCmd = String(template?.command || resolveNodeCommand());
  const servers = {};
  const count = Math.max(1, Number(channelCount) || 1);
  for (let i = 1; i <= count; i++) {
    servers[`qtwx-mcp-${i}`] = {
      command: nodeCmd,
      args: [scriptPath],
      env: {
        QINGTIAN_SESSION: String(i),
        QINGTIAN_QUEUE_ROOT: queueRoot,
        QINGTIAN_RUNTIME_STAMP: stamp
      }
    };
  }
  return servers;
}

function listQingTianMcpIds(mcpServers) {
  return Object.keys(mcpServers || {})
    .filter((k) => /^qtwx-mcp-\d+$/.test(k))
    .map((k) => ({ key: k, id: parseInt(k.replace("qtwx-mcp-", ""), 10) }))
    .filter((x) => Number.isFinite(x.id) && x.id >= 1)
    .sort((a, b) => a.id - b.id);
}

/**
 * Cursor restarts stdio MCP on any mcp.json rewrite (reason=config_changed).
 * Default is FREEZE: never touch existing qtwx entries mid-session.
 * - freeze: create only if missing; never rewrite live entries
 * - channels: surgically add/remove keys; preserve env/args/stamp of survivors
 * - force: full rewrite (manual "刷新配置" only)
 */
function syncWorkspaceMcpConfig(options = {}) {
  if (!_mcpServer) return { ok: false, message: "mcpServer 未加载" };
  const mode = options.mode === "force" || options.mode === "channels" ? options.mode : "freeze";
  const reason = String(options.reason || mode);
  const configFile = getWorkspaceMcpConfigFile();
  if (!configFile) return { ok: false, message: "请先打开工作区" };

  const existing = readJsonSafe(configFile, { mcpServers: {} });
  if (!existing.mcpServers || typeof existing.mcpServers !== "object") {
    existing.mcpServers = {};
  }

  const channelCount = Math.max(1, Number(_mcpServer.getChannelCount()) || 1);
  const before = listQingTianMcpIds(existing.mcpServers);
  const template = before.length
    ? existing.mcpServers[before[0].key]
    : null;

  let nextServers = { ...existing.mcpServers };
  let mutated = false;

  if (mode === "force") {
    const kept = stripQingTianMcpServers(existing.mcpServers);
    nextServers = { ...kept, ...buildQingTianMcpServers(channelCount, template) };
    mutated = true;
  } else if (mode === "channels") {
    // Keep survivors byte-stable; only add missing / drop extras.
    for (let i = 1; i <= channelCount; i++) {
      const key = `qtwx-mcp-${i}`;
      if (nextServers[key]) continue;
      const base = template || buildQingTianMcpServers(1)["qtwx-mcp-1"];
      nextServers[key] = {
        command: base.command,
        args: [...(base.args || [])],
        env: {
          ...(base.env || {}),
          QINGTIAN_SESSION: String(i)
        }
      };
      mutated = true;
    }
    for (const { key, id } of listQingTianMcpIds(nextServers)) {
      if (id > channelCount) {
        delete nextServers[key];
        mutated = true;
      }
    }
  } else {
    // freeze: only bootstrap when nothing usable exists
    const have = listQingTianMcpIds(nextServers);
    const missing = [];
    for (let i = 1; i <= channelCount; i++) {
      if (!nextServers[`qtwx-mcp-${i}`]) missing.push(i);
    }
    if (have.length === 0) {
      nextServers = {
        ...stripQingTianMcpServers(nextServers),
        ...buildQingTianMcpServers(channelCount)
      };
      mutated = true;
      console.log("[SlashSubs] mcp.json bootstrap (first install), channels=", channelCount);
    } else if (missing.length) {
      // Fill gaps without rewriting existing live servers
      const base = template || buildQingTianMcpServers(1)["qtwx-mcp-1"];
      for (const i of missing) {
        const key = `qtwx-mcp-${i}`;
        nextServers[key] = {
          command: base.command,
          args: [...(base.args || [])],
          env: {
            ...(base.env || {}),
            QINGTIAN_SESSION: String(i)
          }
        };
      }
      mutated = true;
      console.log("[SlashSubs] mcp.json freeze-fill missing channels:", missing.join(","));
    } else {
      console.log("[SlashSubs] mcp.json FROZEN — skip rewrite (", reason, ")");
    }
  }

  existing.mcpServers = nextServers;
  const wrote = mutated ? writeJsonAtomicIfChanged(configFile, existing) : false;
  if (wrote) {
    console.warn(
      `[SlashSubs] mcp.json updated (mode=${mode}, reason=${reason}) — Cursor may restart MCP (config_changed)`
    );
  }

  // Only touch global mcp.json on force — avoid extra Cursor reloads mid-session.
  if (mode === "force") {
    const globalFile = path.join(os.homedir(), ".cursor", "mcp.json");
    const globalCfg = readJsonSafe(globalFile, null);
    if (globalCfg?.mcpServers) {
      const cleaned = stripQingTianMcpServers(globalCfg.mcpServers);
      if (Object.keys(cleaned).length !== Object.keys(globalCfg.mcpServers).length) {
        globalCfg.mcpServers = cleaned;
        writeJsonAtomic(globalFile, globalCfg);
      }
    }
  }

  return {
    ok: true,
    configFile,
    channelCount,
    changed: wrote === true,
    mode,
    reason,
    frozen: mode === "freeze" && wrote !== true
  };
}

function writeCursorRules(channelCount) {
  const folders = vscode.workspace.workspaceFolders;
  if (!folders?.length) return;
  const wsPath = folders[0].uri.fsPath;
  const rulesDir = path.join(wsPath, ".cursor", "rules");
  ensureDir(rulesDir);
  const channels = Array.from({ length: channelCount }, (_, i) => `qtwx-mcp-${i + 1}`).join("、");
  const body = `---
description: SlashSubs MCP 持续对话规则；正文在 Cursor 正常回复，并用当前通道 check_messages 保持循环
globs: *
alwaysApply: true
---

# SlashSubs MCP 持续对话规则（多通道）

本工作区已注册 ${channelCount} 个通道：${channels}。通道可在侧栏按需新增，不设固定上限。

你正在使用 SlashSubs MCP。正常回答用户，同时在每轮回答后继续等待插件侧的新消息。

## 核心规则

1. 正常回答用户，不要用套话代替回答。
2. 用户要看的正文写在 Cursor 对话窗口。
3. 每轮完整可见回复结束后，先调用当前通道 \`record_reply({ content })\`，再调用 \`check_messages()\`。
4. 当前处于单窗口模式，不要跨通道调用。
5. 保活/超时信号不要展示给用户，继续 \`check_messages()\`。

## 标准流程

收到插件消息 -> 完整回答用户 -> \`record_reply\` -> \`check_messages\` -> 等待下一条。
`;
  fs.writeFileSync(path.join(rulesDir, "qingtian-mcp.mdc"), body, "utf8");
}

function refreshRuntimeConfig(reason) {
  if (!_mcpServer) return { ok: false, message: "mcpServer 未加载" };
  const why = String(reason || "manual-refresh");
  const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || "";
  if (ws) _mcpServer.writeWorkspaceInfo(ws);
  const cfg = vscode.workspace.getConfiguration("qingtian");
  _mcpServer.writeRuntimeConfig({
    keepaliveEnabled: cfg.get("keepaliveEnabled", true),
    keepaliveMinutes: cfg.get("keepaliveMinutes", 45),
    bridgeEnabled: cfg.get("bridgeEnabled", false),
    bridgeChannel: cfg.get("bridgeChannel", 1),
    bridgeBotToken: cfg.get("bridgeBotToken", ""),
    bridgeUseProxy: _mcpServer.getBridgeUseProxy(),
    agentTeamEnabled: _mcpServer.getPluginSettings().agentTeamEnabled === true
  });

  // mcp.json policy: Cursor kills stdio on any rewrite. Stay frozen unless
  // the user explicitly changes channel count or clicks 刷新配置.
  let mode = "freeze";
  if (why === "restart-server" || why === "manual-refresh" || why === "force") {
    mode = "force";
  } else if (why === "add-channel" || why === "remove-channel") {
    mode = "channels";
  }
  const synced = syncWorkspaceMcpConfig({ mode, reason: why });
  writeCursorRules(_mcpServer.getChannelCount());
  try {
    if (synced?.changed) {
      _statusPayload?.noteRuntimeConfigRefreshNeeded?.(why);
    } else {
      _statusPayload?.clearRuntimeConfigRefreshNeeded?.();
    }
  } catch {}
  return synced;
}

function restartBridgeProcess() {
  try {
    if (_bridgeChild && !_bridgeChild.killed) _bridgeChild.kill();
  } catch {}
  _bridgeChild = null;
  const cfg = vscode.workspace.getConfiguration("qingtian");
  if (!cfg.get("bridgeEnabled", false)) return;
  const token = String(cfg.get("bridgeBotToken", "") || "").trim();
  if (!token || !_mcpServer) return;
  const bridgeEntry = path.join(getExtensionPath(), "bridge", "index.mjs");
  if (!fs.existsSync(bridgeEntry)) return;
  try {
    _bridgeChild = cp.spawn(resolveNodeCommand(), [bridgeEntry], {
      cwd: path.dirname(bridgeEntry),
      env: {
        ...process.env,
        QINGTIAN_QUEUE_ROOT: _mcpServer.getQueueRoot(),
        QINGTIAN_BRIDGE_TOKEN: token,
        QINGTIAN_BRIDGE_CHANNEL: String(cfg.get("bridgeChannel", 1) || 1),
        QINGTIAN_BRIDGE_USE_PROXY: _mcpServer.getBridgeUseProxy() ? "1" : "0"
      },
      stdio: "ignore",
      detached: true
    });
    _bridgeChild.unref();
  } catch (e) {
    console.warn("[QingTian] bridge spawn failed:", e);
  }
}

async function openQingTianPanel() {
  try {
    await vscode.commands.executeCommand("qingtian.panel.focus");
  } catch {
    try {
      await vscode.commands.executeCommand("workbench.view.extension.qingtian-container");
    } catch {}
  }
  _provider?.focusPanel?.();
}

async function addChannel() {
  const result = _mcpServer.tryIncrementChannelCount();
  refreshRuntimeConfig("add-channel");
  pushStatus();
  return { ok: true, newCount: result.newCount, channelCount: result.newCount };
}

async function removeChannel() {
  const current = _mcpServer.getChannelCount();
  if (current <= 1) return { ok: false, error: "至少保留 1 个通道" };
  _mcpServer.setChannelCount(current - 1);
  refreshRuntimeConfig("remove-channel");
  pushStatus();
  return { ok: true, newCount: current - 1, channelCount: current - 1 };
}

function pushStatus() {
  try {
    if (!_provider || !_statusPayload) return;
    _provider.postMessage?.({
      command: "status",
      data: _statusPayload.buildStatusPayload(getVersion())
    });
  } catch (e) {
    console.warn("[QingTian] pushStatus failed:", e);
  }
}

function showBootError(err) {
  const msg = err instanceof Error ? err.stack || err.message : String(err);
  console.error("[QingTian] boot error:", msg);
  try {
    vscode.window.showErrorMessage(`SlashSubs MCP 启动异常: ${err instanceof Error ? err.message : String(err)}`);
  } catch {}
}

async function activate(context) {
  _context = context;
  const extensionUri = context.extensionUri;

  // 1) Load critical modules first (must succeed for UI)
  _activation = safeRequire("./activation");
  _mcpServer = safeRequire("./mcpServer");
  const webviewMod = safeRequire("./webviewProvider");
  if (!_activation || !_mcpServer || !webviewMod?.DialogWebviewProvider) {
    const missing = [
      !_activation && "activation",
      !_mcpServer && "mcpServer",
      !webviewMod?.DialogWebviewProvider && "webviewProvider"
    ]
      .filter(Boolean)
      .join(", ");
    showBootError(new Error(`关键模块加载失败: ${missing}。请确认扩展 node_modules 完整（需要 ws）。`));
    return;
  }
  const { DialogWebviewProvider } = webviewMod;

  // Optional modules — never block sidebar registration
  _statusPayload = safeRequire("./statusPayload");
  const seamlessSwitch = safeRequire("./seamlessSwitch");
  const updateNoticeMod = safeRequire("./updateNotice");
  const batchRetryMod = safeRequire("./batchRetryEngine");
  const externalWebServer = safeRequire("./externalWebServer");
  const cleanupUninstall = safeRequire("./cleanupUninstall");
  const agentTeamWorkbench = safeRequire("./agentTeamWorkbench");

  try {
    await _activation.initEncryptionKey?.();
  } catch (e) {
    console.warn("[QingTian] initEncryptionKey skipped:", e);
  }

  try {
    const storageRoot =
      context.storageUri?.fsPath ||
      context.globalStorageUri?.fsPath ||
      path.join(os.homedir(), ".cursor", "qingtian-runtime");
    _mcpServer.initRuntimePaths(storageRoot);
    _mcpServer.initGlobalState(context.globalState);
    _mcpServer.deployMCPServer(context.extensionPath);
    // Do NOT rewrite mcp.json on activate — Cursor treats that as config_changed
    // and stops live stdio (breaks check_messages / burns quota on restart).
    // Queue path alignment is handled in-memory via resolveCompatibleStorageRoot.
    try {
      const boot = syncWorkspaceMcpConfig({ mode: "freeze", reason: "activate-boot" });
      console.log(
        "[SlashSubs] activate mcp freeze:",
        boot?.frozen ? "kept existing" : boot?.changed ? "bootstrapped" : "ok",
        "channels=",
        boot?.channelCount
      );
    } catch (e) {
      console.warn("[SlashSubs] activate mcp freeze failed:", e);
    }
  } catch (e) {
    showBootError(e);
  }

  try {
    const ok = await _activation.checkActivation();
    console.log("[QingTian] license check:", ok, _activation.getLicenseCountdownStatus?.());
  } catch (e) {
    console.warn("[QingTian] checkActivation failed, forcing local trial:", e);
    try {
      _activation.startLocalTrial?.();
    } catch {}
  }

  const theme = context.globalState.get("qingtian.uiTheme") === "dark" ? "dark" : "light";
  const sendMode = context.globalState.get("qingtian.sendMode") === "ctrl-enter" ? "ctrl-enter" : "enter";
  let lang = context.globalState.get("qingtian.uiLanguage");
  if (lang !== "zh" && lang !== "en") {
    lang = "en";
    try { await context.globalState.update("qingtian.uiLanguage", "en"); } catch {}
  }
  try {
    agentTeamWorkbench?.setAgentTeamWorkbenchLanguage?.(lang);
    agentTeamWorkbench?.setAgentTeamWorkbenchTheme?.(theme);
    agentTeamWorkbench?.setAgentTeamWorkbenchSendMode?.(sendMode);
  } catch {}

  if (updateNoticeMod?.UpdateNoticeManager) {
    try {
      _updateNotice = new updateNoticeMod.UpdateNoticeManager(context, {
        show: (payload) => _provider?.postMessage?.({ command: "updateNotice", ...payload }),
        close: (id) => _provider?.postMessage?.({ command: "updateNoticeClose", noticeId: id })
      });
    } catch (e) {
      console.warn("[QingTian] UpdateNoticeManager failed:", e);
    }
  }

  // 2) Create + register sidebar ASAP
  try {
    _provider = new DialogWebviewProvider(
      extensionUri,
      {
        toggleSuppress: (checked) => {
          try {
            _updateNotice?.setSuppressForCurrent?.(!!checked);
          } catch {
            if (_updateNotice) _updateNotice.suppressOnNextStartup = !!checked;
          }
        },
        openAction: async () => {
          try {
            await _updateNotice?.openCurrentAction?.();
          } catch {
            const url = _updateNotice?.currentActionUrl;
            if (url) await vscode.env.openExternal(vscode.Uri.parse(url));
          }
        },
        close: async () => {
          await _updateNotice?.closeCurrentNotice?.(false);
        }
      },
      async (nextTheme) => {
        await context.globalState.update("qingtian.uiTheme", nextTheme);
        agentTeamWorkbench?.setAgentTeamWorkbenchTheme?.(nextTheme);
      },
      async (nextMode) => {
        await context.globalState.update("qingtian.sendMode", nextMode);
        agentTeamWorkbench?.setAgentTeamWorkbenchSendMode?.(nextMode);
      },
      sendMode
    );
  } catch (e) {
    showBootError(e);
    return;
  }

  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider(DialogWebviewProvider.viewType, _provider, {
      webviewOptions: { retainContextWhenHidden: true }
    })
  );

  // Optional engines after UI is registered
  if (batchRetryMod?.BatchRetryEngine) {
    try {
      _batchRetryEngine = new batchRetryMod.BatchRetryEngine(
        () => _statusPayload?.buildStatusPayload?.(getVersion()) || {},
        (msg, details) => console.log("[QingTian][BatchRetry]", msg, details || "")
      );
      _provider.setBatchRetryEngine?.(_batchRetryEngine);
      seamlessSwitch?.registerBatchEngine?.(_batchRetryEngine);
    } catch (e) {
      console.warn("[QingTian] BatchRetryEngine init failed:", e);
    }
  }

  try {
    agentTeamWorkbench?.registerAgentTeamWorkbenchSerializer?.(context);
  } catch {}

  context.subscriptions.push(
    vscode.commands.registerCommand("qingtian.openPanel", () => openQingTianPanel()),
    vscode.commands.registerCommand("qingtian.activate", async () => {
      await openQingTianPanel();
      const status = _activation.getLicenseCountdownStatus?.();
      if (!status?.activated) {
        vscode.window.showInformationMessage("试用/订阅未生效或已到期：请在侧栏完成 USDT 付款或开始试用。");
      } else {
        vscode.window.showInformationMessage("当前授权有效，无需激活码。");
      }
    }),
    vscode.commands.registerCommand("qingtian.copyStartPrompt", async (channelId, options) => {
      const ch = String(channelId || "1");
      const gate = _mcpServer.assertCanStartNewSession?.(ch, { force: options?.force === true }) || { ok: true };
      if (!gate.ok) {
        return { ok: false, blocked: true, guard: gate.guard, message: gate.message };
      }
      const prepared = _mcpServer.prepareStartPrompt(ch);
      await vscode.env.clipboard.writeText(prepared.prompt);
      return { ...prepared, ok: true, forced: !!gate.forced };
    }),
    vscode.commands.registerCommand("qingtian.sendStartPrompt", async (channelId, options) => {
      const ch = String(channelId || "1");
      const opts = options || {};
      const gate = _mcpServer.assertCanStartNewSession?.(ch, { force: opts.force === true }) || { ok: true };
      if (!gate.ok) {
        return { ok: false, blocked: true, guard: gate.guard, message: gate.message };
      }
      const prepared = _mcpServer.prepareStartPrompt(ch);
      if (!seamlessSwitch?.sendStartPromptToCursor) {
        await vscode.env.clipboard.writeText(prepared.prompt);
        return { ok: false, message: "已复制开场语（无感发送模块未加载）", forced: !!gate.forced };
      }
      const result = await seamlessSwitch.sendStartPromptToCursor(prepared.prompt, prepared.channelId, opts);
      return { ...result, forced: !!gate.forced };
    }),
    vscode.commands.registerCommand("qingtian.stopChannelTurn", async (channelId) => {
      const ch = String(channelId || "1");
      const stopped = _mcpServer.stopChannelTurn?.(ch, { clearQueue: true }) || {
        ok: false,
        message: "stopChannelTurn unavailable"
      };
      // Best-effort: stop Cursor composer generation on the same session (no new start).
      try {
        const engine = _batchRetryEngine;
        if (engine?.requestComposerStopForChannel) {
          await engine.requestComposerStopForChannel(ch, "user_stop_turn");
        }
      } catch (e) {
        console.warn("[SlashSubs] composer stop request skipped:", e);
      }
      const stopCmds = [
        "composer.cancelChatGeneration",
        "aichat.stopgeneration",
        "workbench.action.chat.stop",
        "inlineChat.stop"
      ];
      for (const cmd of stopCmds) {
        try {
          await vscode.commands.executeCommand(cmd);
          break;
        } catch {
          /* try next */
        }
      }
      return stopped;
    }),
    vscode.commands.registerCommand("qingtian.resumeLoop", async (channelId) => {
      const ch = String(channelId || "1");
      const nudged = _mcpServer.nudgeResumeLoop?.(ch);
      if (!nudged?.ok) {
        return nudged || { ok: false, message: "拉回循环失败" };
      }
      // Prefer same bound composer; do not open a brand-new composer.
      if (seamlessSwitch?.sendStartPromptToCursor) {
        const injected = await seamlessSwitch.sendStartPromptToCursor(nudged.prompt, ch, {
          openComposer: false,
          targetMode: "bound"
        });
        return {
          ...nudged,
          injected: !!injected?.ok,
          injectMessage: injected?.message || "",
          message: injected?.ok
            ? `已在 CH-${ch} 原绑定对话投递拉回指令（未新开场）。`
            : (nudged.message + "（绑定对话投递失败时，仅队列生效；请在原对话继续）")
        };
      }
      return nudged;
    }),
    vscode.commands.registerCommand("qingtian.restartServer", async () => {
      const guards = _mcpServer.getAllChannelKeepaliveGuards?.() || {};
      const blocking = Object.values(guards).filter((g) => g && g.blockNewStart);
      if (blocking.length) {
        const ids = blocking.map((g) => `CH-${g.channelId}`).join(", ");
        const sel = await vscode.window.showWarningMessage(
          `${ids} 仍在保活/可能存活。刷新配置可能重启 MCP 并打断同一连接（导致需重新开场耗额度）。仍要刷新吗？`,
          { modal: true },
          "仍要刷新",
          "取消"
        );
        if (sel !== "仍要刷新") {
          return { ok: false, cancelled: true, message: "已取消刷新，以保护当前连接" };
        }
      }
      const synced = refreshRuntimeConfig("restart-server");
      restartBridgeProcess();
      vscode.window.showInformationMessage(
        synced.ok
          ? synced.changed
            ? `配置已强制刷新（${synced.channelCount} 通道）。Cursor 可能重启 MCP；请在同一 Composer 内拉回循环，勿新开场。`
            : `配置未改动（已冻结 mcp.json，${synced.channelCount} 通道）。`
          : synced.message || "刷新失败"
      );
      return synced;
    }),
    vscode.commands.registerCommand("qingtian.addChannel", () => addChannel()),
    vscode.commands.registerCommand("qingtian.removeChannel", () => removeChannel()),
    vscode.commands.registerCommand("qingtian.enableSeamlessSwitch", async () => {
      if (!seamlessSwitch?.injectWorkbench) {
        vscode.window.showWarningMessage("无感切号模块未加载");
        return { ok: false };
      }
      const res = await seamlessSwitch.injectWorkbench();
      vscode.window.showInformationMessage(res?.message || (res?.ok ? "注入完成" : "注入失败"));
      return res;
    }),
    vscode.commands.registerCommand("qingtian.restoreWorkbench", async () => {
      if (!seamlessSwitch?.restoreWorkbench) {
        vscode.window.showWarningMessage("无感切号模块未加载");
        return { ok: false };
      }
      const res = await seamlessSwitch.restoreWorkbench();
      vscode.window.showInformationMessage(res?.message || (res?.ok ? "已还原" : "还原失败"));
      return res;
    }),
    vscode.commands.registerCommand("qingtian.fullCleanup", async () => {
      if (!cleanupUninstall?.performFullCleanup) {
        vscode.window.showWarningMessage("清理模块未加载");
        return;
      }
      const report = await cleanupUninstall.performFullCleanup(context);
      const md = cleanupUninstall.formatCleanupReport(report);
      const doc = await vscode.workspace.openTextDocument({ content: md, language: "markdown" });
      await vscode.window.showTextDocument(doc, { preview: true });
      return report;
    }),
    vscode.commands.registerCommand("qingtian.openWebInBrowser", async () => {
      const info = externalWebServer?.getWebServerInfo?.();
      if (info?.url) {
        await vscode.env.openExternal(vscode.Uri.parse(info.url));
      } else {
        vscode.window.showWarningMessage("Web 服务未运行");
      }
    }),
    vscode.commands.registerCommand("qingtian.openAgentTeamWorkbench", async () => {
      if (!agentTeamWorkbench?.openAgentTeamWorkbench) {
        vscode.window.showWarningMessage("Agent Team 模块未加载");
        return;
      }
      await agentTeamWorkbench.openAgentTeamWorkbench(context);
    })
  );

  // Optional local web UI
  try {
    const cfg = vscode.workspace.getConfiguration("qingtian");
    if (externalWebServer?.startWebServer && cfg.get("webServerEnabled", true) !== false) {
      externalWebServer.setChannelConnectionProvider?.(() => _mcpServer.getMCPStatus());
      await externalWebServer.startWebServer({
        extensionPath: context.extensionPath,
        port: cfg.get("webServerPort", 3180)
      });
    }
  } catch (e) {
    console.warn("[QingTian] web server start failed:", e);
  }

  try {
    if (vscode.workspace.workspaceFolders?.length) {
      refreshRuntimeConfig("activate");
    }
  } catch (e) {
    console.warn("[QingTian] refreshRuntimeConfig failed:", e);
  }

  context.subscriptions.push(
    vscode.workspace.onDidChangeWorkspaceFolders(() => {
      try {
        refreshRuntimeConfig("workspace-changed");
      } catch {}
    })
  );

  try {
    restartBridgeProcess();
  } catch {}

  _statusTimer = setInterval(() => {
    try {
      if (!_provider || !_activation?.isActivated?.()) return;
      pushStatus();
    } catch {}
  }, 5000);
  context.subscriptions.push({
    dispose: () => {
      try {
        clearInterval(_statusTimer);
      } catch {}
    }
  });

  console.log(`[SlashSubs] activated v${getVersion()} (clean entry, default English)`);
}

function deactivate() {
  try {
    clearInterval(_statusTimer);
  } catch {}
  try {
    _activation?.stopPeriodicVerify?.();
  } catch {}
  try {
    safeRequire("./externalWebServer")?.stopWebServer?.();
  } catch {}
  try {
    safeRequire("./seamlessSwitch")?.stopServer?.();
  } catch {}
  try {
    if (_bridgeChild && !_bridgeChild.killed) _bridgeChild.kill();
  } catch {}
  _bridgeChild = null;
  _provider = null;
  _context = null;
}

module.exports = {
  activate,
  deactivate,
  getExtensionContext,
  restartBridgeProcess
};
