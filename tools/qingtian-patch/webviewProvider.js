"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.DialogWebviewProvider = void 0;
const vscode = __importStar(require("vscode"));
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
const os = __importStar(require("os"));
const cp = __importStar(require("child_process"));
const mcpServer_1 = require("./mcpServer");
const statusPayload_1 = require("./statusPayload");
const cursorUsage_1 = require("./cursorUsage");
const cursorAccounts_1 = require("./cursorAccounts");
const seamlessSwitch_1 = require("./seamlessSwitch");
const agentTranscriptIo_1 = require("./agentTranscriptIo");
const quickCommands_1 = require("./quickCommands");
const DEFAULT_BATCH_RETRY_INTERVAL_MS = 300;
const MIN_BATCH_RETRY_INTERVAL_MS = 100;
const MAX_BATCH_RETRY_INTERVAL_MS = 30000;
const DEFAULT_BATCH_RETRY_PORT = 26399;
const DEFAULT_BATCH_RETRY_MAX_RETRIES = Number.MAX_SAFE_INTEGER;
const BATCH_RETRY_HISTORY_KEY = 'qingtian.batchRetryHistory';
const BATCH_RETRY_HISTORY_MAX = 500;
const CURSOR_DISABLE_HTTP2_KEY = 'cursor.general.disableHttp2';
const CURSOR_DISABLE_HTTP1_SSE_KEY = 'cursor.general.disableHttp1SSE';
const PASTE_IMAGE_TMP_DIR = path.join(os.tmpdir(), 'qingtian-images');
const PASTE_IMAGE_CHUNK_TTL_MS = 2 * 60 * 1000;
function trySystemNotify(title, message) {
    if (process.platform !== 'win32')
        return;
    try {
        const nn = require('node-notifier');
        nn.notify({ title, message, sound: true });
    }
    catch (e) {
        console.warn('[QingTian] 系统通知不可用:', e);
    }
}
function isEnglishUi() {
    try {
        const ctx = require('./extension').getExtensionContext?.();
        return ctx?.globalState?.get?.('qingtian.uiLanguage', 'en') !== 'zh';
    }
    catch {
        return false;
    }
}
function uiText(text) {
    const value = String(text || '');
    if (!isEnglishUi())
        return value;
    const map = {
        '授权校验失败: ': 'Authorization check failed: ',
        '晴天无限MCP': 'SlashSubs',
        '发送消息失败': 'Failed to send message',
        '复制开场失败：': 'Failed to copy starter: ',
        '自动恢复上下文失败：': 'Auto restore context failed: ',
        '未知错误': 'Unknown error',
        '回复通知已开启，AI 处理完毕时将弹出提醒': 'Reply notifications enabled. A notification appears when AI finishes.',
        '正在切换账号，Cursor 将自动重启...': 'Switching account. Cursor will restart automatically...',
        '正在重启切号，Cursor 将关闭后自动重新打开...': 'Restart switching. Cursor will close and reopen automatically...',
        '账号列表已复制到剪贴板': 'Account list copied to clipboard',
        '当前 Token 已复制到剪贴板': 'Current token copied to clipboard',
        '确定要退出当前授权吗？退出后需重新试用或付款开通。': 'Deactivate this license? You will need to start a trial or pay again.',
        '确认退出': 'Confirm deactivate',
        '已退出激活，请重新加载窗口。': 'Deactivated. Reload the window.',
        '重新加载': 'Reload',
        '选择文件': 'Select files',
        '选择文件夹': 'Select folder',
        '暂无对话记录可导出': 'No conversation history to export',
        '导出失败: ': 'Export failed: ',
        '粘贴图片失败: ': 'Paste image failed: ',
        '获取 Token 失败': 'Failed to get token'
    };
    if (map[value])
        return map[value];
    let m = value.match(/^通道\s+(.+)\s+开头语已复制到剪贴板$/);
    if (m)
        return 'Starter for channel ' + m[1] + ' copied to clipboard';
    m = value.match(/^CH-(.+)\s+恢复上下文已投递；当前或下一个绑定该通道的 Cursor 窗口会自动接手。$/);
    if (m)
        return 'CH-' + m[1] + ' restore context delivered. The current or next Cursor window bound to this channel will take over automatically.';
    m = value.match(/^已将 CH-(.+) 的上下文转移到 CH-(.+)；目标通道窗口会自动接手。$/);
    if (m)
        return 'Transferred context from CH-' + m[1] + ' to CH-' + m[2] + '. The target channel window will take over automatically.';
    m = value.match(/^已导出\s+(\d+)\s+个账号到剪贴板$/);
    if (m)
        return 'Exported ' + m[1] + ' accounts to clipboard';
    if (value.startsWith('授权校验失败: '))
        return map['授权校验失败: '] + value.slice('授权校验失败: '.length);
    if (value.startsWith('复制开场失败：'))
        return map['复制开场失败：'] + value.slice('复制开场失败：'.length);
    if (value.startsWith('自动恢复上下文失败：'))
        return map['自动恢复上下文失败：'] + value.slice('自动恢复上下文失败：'.length);
    if (value.startsWith('导出失败: '))
        return map['导出失败: '] + value.slice('导出失败: '.length);
    if (value.startsWith('粘贴图片失败: '))
        return map['粘贴图片失败: '] + value.slice('粘贴图片失败: '.length);
    return value;
}
function buildSeamlessStatusPayload() {
    const injectionStatus = (0, seamlessSwitch_1.getSeamlessInjectionStatus)();
    const runtimeStatus = (0, seamlessSwitch_1.getSeamlessRuntimeStatus)();
    const ready = injectionStatus.current && runtimeStatus.runtimeLoaded;
    return {
        command: 'seamlessStatus',
        enabled: ready,
        installed: injectionStatus.injected,
        injectionCurrent: injectionStatus.current,
        injectedRuntimeVersion: injectionStatus.version,
        expectedRuntimeVersion: injectionStatus.expectedVersion,
        runtimeLoaded: runtimeStatus.runtimeLoaded,
        runtimeAnyLoaded: runtimeStatus.runtimeAnyLoaded,
        runtimeCurrent: runtimeStatus.runtimeCurrent,
        runtimeClientCount: runtimeStatus.runtimeClientCount,
        runtimeClients: runtimeStatus.runtimeClients
    };
}
function hasStoppedCheckMessagesEvidence(binding) {
    if (!binding || binding.connectedActive === true || binding.mcpWaitingActive === true || binding.runningCheckActive === true)
        return false;
    const status = String(binding.checkMessagesStatus || '').trim();
    if (status)
        return true;
    return binding.bottomMicButton === true && binding.activeComposerMatches === true;
}
function hasReusableChannelBindingRecord(binding, channelId) {
    const bindingChannelId = String(binding?.channelId || '').trim();
    if (!bindingChannelId || !String(binding?.composerId || '').trim())
        return false;
    if (channelId && bindingChannelId !== String(channelId || '').trim())
        return false;
    return binding.exists !== false;
}
function isReusableChannelBindingBusy(binding) {
    return !!(binding &&
        (binding.connectedActive === true ||
            binding.mcpWaitingActive === true ||
            binding.runningCheckActive === true ||
            binding.visibleGeneratingActive === true));
}
function isCurrentRuntimeChannelBinding(binding) {
    return !!(binding &&
        String(binding.channelId || '').trim() &&
        String(binding.composerId || '').trim() &&
        binding.exists !== false &&
        (binding.runningCheckActive === true ||
            binding.connectedActive === true ||
            binding.mcpWaitingActive === true ||
            binding.runtimeOccupancyOnly === true ||
            /check_messages_scan/i.test(String(binding.connectionReason || binding.source || ''))));
}
function isRuntimeOccupiedChannel(binding) {
    return !!(binding &&
        String(binding.channelId || '').trim() &&
        (binding.connectedActive === true ||
            binding.mcpWaitingActive === true ||
            binding.runningCheckActive === true));
}
function normalizeBatchRetryOpts(raw) {
    const useComposerBridge = raw?.useComposerBridge !== false;
    return {
        sessionCount: Math.max(0, Math.min(50, Number(raw?.sessionCount) || 0)),
        retryInterval: normalizeBatchRetryInterval(raw?.retryInterval),
        useComposerBridge,
        reuseExisting: raw?.reuseExisting === true,
        strictReuseExisting: raw?.strictReuseExisting === true,
        customPort: DEFAULT_BATCH_RETRY_PORT,
        maxRetries: DEFAULT_BATCH_RETRY_MAX_RETRIES,
        parallelWindowDispatch: useComposerBridge,
        customJoinPhraseEnabled: raw?.customJoinPhraseEnabled === true,
        joinPhraseTemplate: String(raw?.joinPhraseTemplate || '')
    };
}
function normalizeBatchRetryInterval(raw) {
    const value = Number(raw);
    if (!Number.isFinite(value))
        return DEFAULT_BATCH_RETRY_INTERVAL_MS;
    return Math.max(MIN_BATCH_RETRY_INTERVAL_MS, Math.min(MAX_BATCH_RETRY_INTERVAL_MS, Math.round(value)));
}
function buildBatchRetryRestartChannelFailure(channelId, error, message, extra) {
    return {
        ok: false,
        channelId: String(channelId || '').trim(),
        error,
        message,
        ...(extra || {})
    };
}
function readMcpServersFromFile(configFile) {
    try {
        if (!fs.existsSync(configFile))
            return [];
        const config = JSON.parse(fs.readFileSync(configFile, 'utf-8'));
        const servers = config?.mcpServers;
        if (!servers || typeof servers !== 'object')
            return [];
        return Object.entries(servers)
            .map(([name, raw]) => ({
            name,
            command: raw && typeof raw === 'object' ? String(raw.command || '') : ''
        }))
            .sort((a, b) => a.name.localeCompare(b.name));
    }
    catch {
        return [];
    }
}
function getWorkspaceMCPConfigFile() {
    const folders = vscode.workspace.workspaceFolders;
    if (!folders || folders.length === 0) {
        return null;
    }
    return path.join(folders[0].uri.fsPath, '.cursor', 'mcp.json');
}
function isQingTianMcpServer(name) {
    return /^qtwx-mcp-\d+$/.test(name);
}
function readWorkspaceMcpServers() {
    const servers = new Map();
    const globalConfigFile = path.join(os.homedir(), '.cursor', 'mcp.json');
    for (const item of readMcpServersFromFile(globalConfigFile)) {
        if (!isQingTianMcpServer(item.name)) {
            servers.set(item.name, item);
        }
    }
    const workspaceConfigFile = getWorkspaceMCPConfigFile();
    if (workspaceConfigFile) {
        for (const item of readMcpServersFromFile(workspaceConfigFile)) {
            servers.set(item.name, item);
        }
    }
    return Array.from(servers.values()).sort((a, b) => a.name.localeCompare(b.name));
}
function buildMcpCallPrefix(rawNames) {
    if (!Array.isArray(rawNames))
        return '';
    const names = rawNames
        .map((item) => String(item || '').trim())
        .filter(Boolean)
        .filter((name, idx, arr) => arr.indexOf(name) === idx);
    if (names.length === 0)
        return '';
    return [
        '【本轮 MCP 调用要求】',
        `用户已勾选希望你在本轮任务中调用以下 MCP：${names.join('、')}`,
        '请根据任务需要优先调用这些 MCP；如果某个 MCP 与本轮任务无关或不可用，请在回复中简要说明。',
        '',
        '【用户原始消息】',
        ''
    ].join('\n');
}
class DialogWebviewProvider {
    constructor(extensionUri, updateNoticeHandlers, themeChanged, sendModeChanged, initialSendMode) {
        this._quickCommandsUnsubscribe = null;
        this._initialSendMode = 'enter';
        this._batchRetryEngine = null;
        this._batchRetryHistoryRecordedTaskIds = new Set();
        this._batchRetryNotifiedTaskIds = new Set();
        this._batchRetryPendingRun = null;
        this._batchRetryRevealClientIds = new Set();
        this._onboardingTriggered = false;
        this._pendingPasteImages = new Map();
        this._extensionUri = extensionUri;
        this._updateNoticeHandlers = updateNoticeHandlers;
        this._themeChanged = themeChanged;
        this._sendModeChanged = sendModeChanged;
        this._initialSendMode = initialSendMode === 'ctrl-enter' ? 'ctrl-enter' : 'enter';
    }
    setBatchRetryEngine(engine) {
        this._batchRetryEngine = engine;
    }
    setBatchRetryLogger(logFn) {
        this._batchRetryLogFn = logFn;
    }
    _logBatchRetry(message, details) {
        try {
            if (this._batchRetryLogFn) {
                this._batchRetryLogFn(message, details);
                return;
            }
        }
        catch { }
        try {
            const suffix = details === undefined ? '' : ' ' + JSON.stringify(details);
            console.log(`[QingTianMCP][BatchRetry] ${message}${suffix}`);
        }
        catch { }
    }
    _resolveCursorLaunchers() {
        const candidates = [];
        const add = (command, source) => {
            const normalized = String(command || '').trim();
            if (!normalized)
                return;
            if (candidates.some(item => item.command.toLowerCase() === normalized.toLowerCase()))
                return;
            try {
                if (fs.existsSync(normalized))
                    candidates.push({ command: normalized, source });
            }
            catch { }
        };
        const execPath = process.execPath || '';
        const execDir = execPath ? path.dirname(execPath) : '';
        if (execDir) {
            add(path.join(execDir, 'resources', 'app', 'bin', process.platform === 'win32' ? 'cursor.cmd' : 'cursor'), 'execPath-relative-cli');
        }
        const resourcesPath = String(process.resourcesPath || '');
        if (resourcesPath) {
            add(path.join(resourcesPath, 'app', 'bin', process.platform === 'win32' ? 'cursor.cmd' : 'cursor'), 'resourcesPath-cli');
        }
        // Prefer the CLI wrapper over Cursor.exe. Cursor.exe may accept the spawn but ignore
        // --new-window when another Cursor process is already running.
        add(execPath, 'process.execPath');
        if (process.platform === 'win32') {
            const localAppData = process.env.LOCALAPPDATA || '';
            if (localAppData) {
                add(path.join(localAppData, 'Programs', 'Cursor', 'resources', 'app', 'bin', 'cursor.cmd'), 'LOCALAPPDATA cursor.cmd');
                add(path.join(localAppData, 'Programs', 'Cursor', 'Cursor.exe'), 'LOCALAPPDATA Cursor.exe');
                add(path.join(localAppData, 'Programs', 'cursor', 'Cursor.exe'), 'LOCALAPPDATA cursor.exe');
            }
            add('C:\\Program Files\\Cursor\\Cursor.exe', 'ProgramFiles Cursor.exe');
            add('C:\\Program Files (x86)\\Cursor\\Cursor.exe', 'ProgramFilesX86 Cursor.exe');
        }
        else if (process.platform === 'darwin') {
            add('/Applications/Cursor.app/Contents/Resources/app/bin/cursor', 'Applications cursor cli');
        }
        else {
            add('/usr/bin/cursor', 'usr-bin cursor');
            add('/usr/local/bin/cursor', 'usr-local-bin cursor');
        }
        return candidates;
    }
    _quoteCmdArg(value) {
        return `"${String(value || '').replace(/"/g, '""')}"`;
    }
    _spawnBatchRetryRevealLauncher(launcher, args, taskId) {
        try {
            const lower = launcher.command.toLowerCase();
            let child;
            if (process.platform === 'win32' && (lower.endsWith('.cmd') || lower.endsWith('.bat'))) {
                const line = [this._quoteCmdArg(launcher.command), ...args.map(arg => this._quoteCmdArg(arg))].join(' ');
                child = cp.spawn('cmd.exe', ['/d', '/s', '/c', line], {
                    detached: true,
                    stdio: 'ignore',
                    windowsHide: true
                });
            }
            else {
                child = cp.spawn(launcher.command, args, {
                    detached: true,
                    stdio: 'ignore',
                    windowsHide: true
                });
            }
            child.once('error', (err) => {
                this._logBatchRetry('reveal cli open child error', {
                    taskId,
                    command: launcher.command,
                    source: launcher.source,
                    args,
                    error: err.message
                });
            });
            child.unref();
            this._logBatchRetry('reveal cli open spawned', {
                taskId,
                command: launcher.command,
                source: launcher.source,
                args
            });
            return { ok: true, command: launcher.command, source: launcher.source, args };
        }
        catch (e) {
            const error = e instanceof Error ? e.message : String(e);
            this._logBatchRetry('reveal cli open failed', {
                taskId,
                command: launcher.command,
                source: launcher.source,
                args,
                error
            });
            return { ok: false, command: launcher.command, source: launcher.source, args, error };
        }
    }
    _buildBatchRetryRevealOpenArgVariants(folderPath) {
        const variants = [];
        const add = (args) => {
            const key = JSON.stringify(args);
            if (!variants.some(item => JSON.stringify(item) === key))
                variants.push(args);
        };
        if (folderPath) {
            add(['--new-window', folderPath]);
            add(['-n', folderPath]);
        }
        else {
            add(['--new-window']);
            add(['-n']);
        }
        return variants;
    }
    async _openAndWaitBatchRetryRevealWindowViaCli(engine, folderUri, existingClientIds, taskId) {
        const folderPath = folderUri?.scheme === 'file' ? folderUri.fsPath : '';
        const launchers = this._resolveCursorLaunchers();
        const argVariants = this._buildBatchRetryRevealOpenArgVariants(folderPath);
        const attempts = [];
        this._logBatchRetry('reveal cli open candidates', {
            taskId,
            folderPath,
            launchers,
            argVariants
        });
        for (const launcher of launchers) {
            for (const args of argVariants) {
                const open = this._spawnBatchRetryRevealLauncher(launcher, args, taskId);
                let clientId = null;
                if (open.ok) {
                    clientId = await this._waitForBatchRetryRevealClient(engine, existingClientIds, 8000, taskId, `cli-fallback:${launcher.source}:${args[0] || 'default'}`);
                }
                attempts.push({ open, clientId });
                this._logBatchRetry('reveal cli open attempt result', {
                    taskId,
                    open,
                    clientId,
                    onlineClientIds: engine.getOnlineClientIds()
                });
                if (clientId)
                    return { clientId, attempts };
            }
        }
        return { clientId: null, attempts };
    }
    async _openBatchRetryRevealWindowViaCli(folderUri, taskId) {
        const folderPath = folderUri?.scheme === 'file' ? folderUri.fsPath : '';
        const launchers = this._resolveCursorLaunchers();
        const args = ['--new-window'].concat(folderPath ? [folderPath] : []);
        const tried = [];
        this._logBatchRetry('reveal cli open candidates', {
            taskId,
            folderPath,
            launchers
        });
        for (const launcher of launchers) {
            const open = this._spawnBatchRetryRevealLauncher(launcher, args, taskId);
            if (open.ok) {
                return { ok: true, command: open.command, source: open.source, args: open.args };
            }
            else {
                const error = open.error || 'spawn_failed';
                tried.push({ command: launcher.command, source: launcher.source, error });
            }
        }
        return { ok: false, error: 'cursor_launcher_not_found_or_failed', tried };
    }
    async _waitForBatchRetryRevealClient(engine, existingClientIds, timeoutMs, taskId, phase) {
        const existing = new Set(Array.from(existingClientIds).map(clientId => String(clientId || '').trim()).filter(Boolean));
        const started = Date.now();
        this._logBatchRetry('reveal wait usable new client begin', {
            taskId,
            phase,
            existingClientIds: Array.from(existing),
            reservedRevealClientIds: Array.from(this._batchRetryRevealClientIds),
            timeoutMs,
            onlineClientIds: engine.getOnlineClientIds()
        });
        while (Date.now() - started < timeoutMs) {
            const onlineClientIds = engine.getOnlineClientIds();
            const newcomer = onlineClientIds.find(clientId => !existing.has(clientId) && !this._batchRetryRevealClientIds.has(clientId));
            if (newcomer) {
                this._batchRetryRevealClientIds.add(newcomer);
                this._logBatchRetry('reveal wait usable new client detected', {
                    taskId,
                    phase,
                    newClientId: newcomer,
                    elapsedMs: Date.now() - started,
                    onlineClientIds,
                    reservedRevealClientIds: Array.from(this._batchRetryRevealClientIds)
                });
                return newcomer;
            }
            await new Promise(resolve => setTimeout(resolve, 250));
        }
        this._logBatchRetry('reveal wait usable new client timeout', {
            taskId,
            phase,
            elapsedMs: Date.now() - started,
            onlineClientIds: engine.getOnlineClientIds(),
            reservedRevealClientIds: Array.from(this._batchRetryRevealClientIds)
        });
        return null;
    }
    async _adoptFocusedBatchWindow(reason) {
        const engine = this._batchRetryEngine;
        if (!engine)
            return { ok: false, error: 'engine_not_ready' };
        const bridgeOk = await engine.ensureHttpBridgeReady();
        const port = engine.getBridgePort();
        if (!bridgeOk || !port) {
            return { ok: false, port, error: bridgeOk ? 'bridge_port_missing' : 'bridge_unavailable' };
        }
        try {
            const adoptedAt = Date.now();
            const result = await (0, seamlessSwitch_1.adoptFocusedWindowForBatch)(port, engine.getWorkspacePath());
            this._logBatchRetry('focused batch window adopted', {
                reason,
                ok: result.ok,
                clientId: result.clientId || '',
                adoptedAt,
                port,
                workspacePath: engine.getWorkspacePath()
            });
            return { ok: result.ok, clientId: result.clientId || '', port, adoptedAt };
        }
        catch (e) {
            const error = e instanceof Error ? e.message : String(e);
            this._logBatchRetry('focused batch window adopt failed', {
                reason,
                port,
                error
            });
            return { ok: false, port, error };
        }
    }
    async _focusedBatchTargetOpts(reason) {
        const adoption = await this._adoptFocusedBatchWindow(reason);
        const targetClientId = adoption.ok && adoption.clientId ? String(adoption.clientId) : '';
        const targetClientSeenAfter = targetClientId ? (Number(adoption.adoptedAt || 0) || 0) : 0;
        return {
            targetClientId,
            targetClientSeenAfter,
            opts: targetClientId ? { targetClientId, targetClientSeenAfter } : undefined
        };
    }
    async _retryFocusedBatchBindingOp(reason, run) {
        let target = await this._focusedBatchTargetOpts(reason);
        let result = await run(target.opts);
        if (String(result?.error || '') !== 'no_active_client')
            return result;
        this._logBatchRetry('focused binding op no active client, retrying adoption', {
            reason,
            targetClientId: target.targetClientId,
            targetClientSeenAfter: target.targetClientSeenAfter
        });
        await new Promise(resolve => setTimeout(resolve, 1200));
        target = await this._focusedBatchTargetOpts(`${reason}_retry`);
        result = await run(target.opts);
        this._logBatchRetry('focused binding op retry result', {
            reason,
            ok: result?.ok === true,
            error: String(result?.error || ''),
            targetClientId: target.targetClientId,
            targetClientSeenAfter: target.targetClientSeenAfter
        });
        return result;
    }
    _probeBatchRetryChannel(channelId, releasedByCheckMessages = false) {
        const ch = String(channelId || '').trim();
        const now = Date.now();
        let heartbeat = null;
        let waiting = null;
        let waitingRawActive = null;
        let waitingUpdatedAt = null;
        let waitingRuntimeStamp = '';
        let waitingPid = null;
        if (ch) {
            try {
                heartbeat = (0, mcpServer_1.readChannelHeartbeat)(ch);
            }
            catch { }
            try {
                waiting = (0, mcpServer_1.readChannelWaiting)(ch, now);
            }
            catch { }
            try {
                const waitingPath = path.join((0, mcpServer_1.getQueueRoot)(), 's', ch, 'waiting.json');
                if (fs.existsSync(waitingPath)) {
                    const raw = JSON.parse(fs.readFileSync(waitingPath, 'utf-8'));
                    waitingRawActive = raw?.active === true;
                    const rawUpdatedAt = Number(raw?.updatedAt || 0);
                    waitingUpdatedAt = Number.isFinite(rawUpdatedAt) && rawUpdatedAt > 0 ? rawUpdatedAt : null;
                    waitingRuntimeStamp = typeof raw?.runtimeStamp === 'string' ? raw.runtimeStamp : '';
                    const rawPid = Number(raw?.pid);
                    waitingPid = Number.isFinite(rawPid) ? rawPid : null;
                }
            }
            catch { }
        }
        const heartbeatLastSeen = heartbeat?.lastSeen && Number.isFinite(Number(heartbeat.lastSeen))
            ? Number(heartbeat.lastSeen)
            : null;
        const heartbeatOnline = ch ? (0, mcpServer_1.isChannelOnline)(ch, now) : false;
        const effectiveWaitingActive = !!waiting && releasedByCheckMessages !== true;
        return {
            channelId: ch,
            heartbeatOnline,
            heartbeatLastSeen,
            heartbeatAgeMs: heartbeatLastSeen ? Math.max(0, now - heartbeatLastSeen) : null,
            heartbeatRuntimeStamp: heartbeat?.runtimeStamp || '',
            heartbeatPid: Number.isFinite(Number(heartbeat?.pid)) ? Number(heartbeat?.pid) : null,
            waitingActive: !!waiting,
            waitingRawActive,
            waitingUpdatedAt,
            waitingAgeMs: waitingUpdatedAt ? Math.max(0, now - waitingUpdatedAt) : null,
            waitingRuntimeStamp,
            waitingPid,
            releasedByCheckMessages: releasedByCheckMessages === true,
            effectiveWaitingActive,
            // heartbeatOnline means the MCP switch/process is available.
            // effectiveWaitingActive keeps hidden sessions occupied unless Cursor shows
            // that this channel's check_messages call has already stopped.
            idle: heartbeatOnline && !effectiveWaitingActive
        };
    }
    _clearBatchRetryPendingRun(clearEngineWait = true) {
        const run = this._batchRetryPendingRun;
        if (run?.timer) {
            clearInterval(run.timer);
        }
        this._batchRetryPendingRun = null;
        if (run) {
            this._logBatchRetry('pending run cleared', {
                runId: run.id,
                clearEngineWait,
                requested: run.requested,
                started: run.started,
                pendingChannelIds: Array.from(run.pendingChannelIds),
                scheduledChannelIds: Array.from(run.scheduledChannelIds),
                reuseBindingChannelIds: Array.from(run.reuseBindingChannelIds || []),
                allowedChannelIds: Array.from(run.allowedChannelIds || []),
                targetClientId: run.targetClientId || '',
                targetClientSeenAfter: run.targetClientSeenAfter || 0,
                addedChannelIds: Array.from(run.addedChannelIds)
            });
        }
        if (clearEngineWait) {
            this._batchRetryEngine?.setChannelWait(null);
        }
        return run || null;
    }
    _makeBatchRetryChannelWaitStatus(run, phase = 'waiting') {
        const pending = Array.from(run.pendingChannelIds).sort((a, b) => Number(a) - Number(b));
        const added = Array.from(run.addedChannelIds).sort((a, b) => Number(a) - Number(b));
        const pendingCount = Math.max(0, pending.length);
        const pendingText = pending.map(id => `CH-${id}`).join('、') || '暂无';
        return {
            id: run.id,
            active: pendingCount > 0,
            phase,
            requested: run.requested,
            started: run.started,
            pending: pendingCount,
            addedChannelIds: added,
            waitingChannelIds: pending,
            message: pendingCount > 0
                ? `空闲通道不足，等待 ${pendingCount} 个通道打开 MCP：${pendingText}`
                : '已补齐批量重试通道'
        };
    }
    _pushBatchRetryPendingStatus(run, channelWaitOverride) {
        const channelWait = channelWaitOverride || this._makeBatchRetryChannelWaitStatus(run);
        this._logBatchRetry('pending status pushed', {
            runId: run.id,
            message: channelWait.message,
            requested: run.requested,
            started: run.started,
            pendingChannelIds: Array.from(run.pendingChannelIds),
            scheduledChannelIds: Array.from(run.scheduledChannelIds),
            addedChannelIds: Array.from(run.addedChannelIds)
        });
        if (this._batchRetryEngine?.isRunning()) {
            this._batchRetryEngine.setChannelWait(channelWait);
            return;
        }
        try {
            this._view?.webview.postMessage({
                command: 'batchRetryStatus',
                data: {
                    text: channelWait.message,
                    count: '0/0',
                    done: false,
                    channelWait,
                    tasks: [],
                    sessionComposerBindings: [],
                    clientCount: 0,
                    parallelWindowDispatch: true
                }
            });
        }
        catch { }
    }
    _isBatchRetryChannelIdle(channelId) {
        return this._probeBatchRetryChannel(channelId).idle;
    }
    async _revealBatchRetryChannel(channelIdRaw) {
        const channelId = String(channelIdRaw || '').trim();
        if (!channelId) {
            return { ok: false, channelId, error: 'invalid_channel' };
        }
        const engine = this._batchRetryEngine;
        if (!engine) {
            return { ok: false, channelId, error: 'engine_not_ready' };
        }
        const probe = this._probeBatchRetryChannel(channelId);
        let bindingResult;
        try {
            bindingResult = await engine.listChannelBindings(8000);
        }
        catch (e) {
            const error = e instanceof Error ? e.message : String(e);
            this._logBatchRetry('channel reveal binding scan exception', { channelId, error, probe });
            return { ok: false, channelId, error: 'binding_scan_failed', details: error, probe };
        }
        const scoreBinding = (binding) => {
            if (!hasReusableChannelBindingRecord(binding, channelId))
                return -100000;
            let score = 0;
            if (isCurrentRuntimeChannelBinding(binding))
                score += 500;
            if (binding?.currentMcpSession === true)
                score += 120;
            if (binding?.runningCheckActive === true)
                score += 120;
            if (binding?.mcpWaitingActive === true)
                score += 90;
            if (binding?.connectedActive === true)
                score += 80;
            if (binding?.runtimeWaitingActive === true)
                score += 60;
            if (binding?.runtimeOccupancyOnly === true)
                score += 50;
            if (/check_messages_scan/i.test(String(binding?.connectionReason || binding?.source || '')))
                score += 45;
            if (binding?.clientMatches === true)
                score += 30;
            if (binding?.observable === true)
                score += 20;
            if (binding?.exists === true)
                score += 10;
            if (binding?.exists === false)
                score -= 500;
            if (String(binding?.clientId || '').trim())
                score += 5;
            const updatedAt = Math.max(Number(binding?.mcpWaitingUpdatedAt || 0) || 0, Number(binding?.updatedAt || 0) || 0);
            if (updatedAt > 0)
                score += Math.max(0, 30 - Math.min(30, Math.floor((Date.now() - updatedAt) / 1000)));
            return score;
        };
        const summarizeBinding = (binding) => ({
            channelId: String(binding?.channelId || ''),
            composerId: String(binding?.composerId || ''),
            clientId: String(binding?.clientId || ''),
            exists: binding?.exists === true,
            clientMatches: binding?.clientMatches === true,
            observable: binding?.observable === true,
            currentMcpSession: binding?.currentMcpSession === true,
            runningCheckActive: binding?.runningCheckActive === true,
            mcpWaitingActive: binding?.mcpWaitingActive === true,
            runtimeWaitingActive: binding?.runtimeWaitingActive === true,
            connectedActive: binding?.connectedActive === true,
            runtimeOccupancyOnly: binding?.runtimeOccupancyOnly === true,
            connectionReason: String(binding?.connectionReason || ''),
            source: String(binding?.source || ''),
            score: scoreBinding(binding)
        });
        const launchedBindings = (engine.getLaunchedSessions() || [])
            .filter((session) => String(session?.channelId || '').trim() === channelId && String(session?.composerId || '').trim())
            .map((session) => ({
            channelId,
            composerId: String(session.composerId || '').trim(),
            clientId: String(session.clientId || '').trim(),
            exists: true,
            clientMatches: true,
            connectedActive: session.isOnline === true,
            source: 'launched_session'
        }));
        const allCandidates = [
            ...(Array.isArray(bindingResult.occupiedChannels) ? bindingResult.occupiedChannels : []),
            ...(Array.isArray(bindingResult.bindings) ? bindingResult.bindings : []),
            ...(Array.isArray(bindingResult.allBindings) ? bindingResult.allBindings : []),
            ...launchedBindings
        ].filter((binding) => hasReusableChannelBindingRecord(binding, channelId));
        const currentCandidates = allCandidates.filter((binding) => isCurrentRuntimeChannelBinding(binding));
        const candidates = probe.effectiveWaitingActive ? currentCandidates : allCandidates;
        const binding = candidates.sort((a, b) => scoreBinding(b) - scoreBinding(a))[0];
        this._logBatchRetry('channel reveal binding scan', {
            channelId,
            ok: bindingResult.ok,
            error: bindingResult.error || '',
            probe,
            bindingCount: bindingResult.bindings?.length || 0,
            allBindingCount: bindingResult.allBindings?.length || 0,
            occupiedCount: bindingResult.occupiedChannels?.length || 0,
            currentCandidateCount: currentCandidates.length,
            chosen: summarizeBinding(binding),
            candidates: allCandidates.map(summarizeBinding)
        });
        if (!binding || !String(binding.composerId || '').trim()) {
            return {
                ok: false,
                channelId,
                error: probe.effectiveWaitingActive ? 'current_channel_binding_not_found' : 'missing_channel_binding',
                probe,
                scan: {
                    ok: bindingResult.ok,
                    error: bindingResult.error || '',
                    bindingCount: bindingResult.bindings?.length || 0,
                    allBindingCount: bindingResult.allBindings?.length || 0,
                    occupiedCount: bindingResult.occupiedChannels?.length || 0
                }
            };
        }
        const composerId = String(binding.composerId || '').trim();
        const clientId = String(binding.clientId || '').trim();
        const targetClientIds = clientId ? [clientId] : undefined;
        const reveal = await engine.revealComposerInNewAgent(composerId, 12000, targetClientIds, { channelId, preferredClientId: clientId });
        return {
            ...reveal,
            channelId,
            composerId,
            clientId,
            binding: summarizeBinding(binding)
        };
    }
    async _restartBatchRetryChannel(channelIdRaw, webview) {
        const channelId = String(channelIdRaw || '').trim();
        const configuredCount = Math.max(1, (0, mcpServer_1.getChannelCount)());
        const numericChannelId = Number(channelId);
        if (!channelId || !Number.isInteger(numericChannelId) || numericChannelId < 1 || numericChannelId > configuredCount) {
            return buildBatchRetryRestartChannelFailure(channelId, 'invalid_channel', '通道不存在，请刷新后再试');
        }
        if (!await this._ensureActivated('单通道会话重试')) {
            return buildBatchRetryRestartChannelFailure(channelId, 'not_activated', '授权已失效，请完成付款后续费后再重试。');
        }
        const platformBlocker = this._getBatchRetryPlatformBlocker();
        if (platformBlocker) {
            return buildBatchRetryRestartChannelFailure(channelId, 'platform_blocked', platformBlocker);
        }
        await (0, seamlessSwitch_1.refreshPrimaryRuntimeProbe)().catch(() => { });
        const injectionStatus = (0, seamlessSwitch_1.getSeamlessInjectionStatus)();
        const runtimeStatus = (0, seamlessSwitch_1.getSeamlessRuntimeStatus)();
        if (!injectionStatus.current || !runtimeStatus.runtimeLoaded) {
            return buildBatchRetryRestartChannelFailure(channelId, 'requires_injection', this._getBatchRetryPreflightMessage(), { requiresInjection: true });
        }
        const engine = this._batchRetryEngine;
        if (!engine) {
            return buildBatchRetryRestartChannelFailure(channelId, 'engine_not_ready', '引擎未初始化');
        }
        if (engine.isRunning() || this._batchRetryPendingRun) {
            return buildBatchRetryRestartChannelFailure(channelId, 'batch_retry_running', '批量会话重试正在运行，稍后再试，避免打断正在工作的通道。');
        }
        const queueLength = (0, mcpServer_1.getQueueLength)(channelId);
        if (queueLength > 0) {
            return buildBatchRetryRestartChannelFailure(channelId, 'queue_not_empty', `CH-${channelId} 还有 ${queueLength} 条待处理消息，请处理完队列后再重试。`, { queueLength });
        }
        const initialProbe = this._probeBatchRetryChannel(channelId);
        if (!initialProbe.heartbeatOnline) {
            return buildBatchRetryRestartChannelFailure(channelId, 'mcp_not_open', `CH-${channelId} 还没打开 MCP 开关，请先在 Cursor MCP 设置里打开该通道。`, { probe: initialProbe });
        }
        if (initialProbe.effectiveWaitingActive) {
            return buildBatchRetryRestartChannelFailure(channelId, 'channel_busy_blue_light', `CH-${channelId} 正在工作/待命中，不能打断；等绿灯后再重试。`, { probe: initialProbe });
        }
        let bindingResult;
        try {
            bindingResult = await engine.listChannelBindings(8000);
        }
        catch (e) {
            const error = e instanceof Error ? e.message : String(e);
            this._logBatchRetry('single channel restart binding scan exception', { channelId, error });
            return buildBatchRetryRestartChannelFailure(channelId, 'binding_scan_failed', `CH-${channelId} 通道重试启动失败：${error}`);
        }
        const bindingSummary = {
            ok: bindingResult.ok,
            error: bindingResult.error || '',
            bindings: (bindingResult.bindings || []).map((binding) => ({
                channelId: String(binding?.channelId || ''),
                composerId: String(binding?.composerId || ''),
                clientId: String(binding?.clientId || ''),
                exists: binding?.exists === true,
                clientMatches: binding?.clientMatches === true,
                mcpWaitingActive: binding?.mcpWaitingActive === true,
                runningCheckActive: binding?.runningCheckActive === true,
                visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                connectedActive: binding?.connectedActive === true,
                checkMessagesStatus: String(binding?.checkMessagesStatus || '')
            })),
            allBindings: (bindingResult.allBindings || []).map((binding) => ({
                channelId: String(binding?.channelId || ''),
                composerId: String(binding?.composerId || ''),
                clientId: String(binding?.clientId || ''),
                exists: binding?.exists === true,
                clientMatches: binding?.clientMatches === true,
                mcpWaitingActive: binding?.mcpWaitingActive === true,
                runningCheckActive: binding?.runningCheckActive === true,
                visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                connectedActive: binding?.connectedActive === true,
                checkMessagesStatus: String(binding?.checkMessagesStatus || '')
            })),
            occupiedChannels: (bindingResult.occupiedChannels || []).map((binding) => ({
                channelId: String(binding?.channelId || ''),
                composerId: String(binding?.composerId || ''),
                clientId: String(binding?.clientId || ''),
                exists: binding?.exists === true,
                clientMatches: binding?.clientMatches === true,
                mcpWaitingActive: binding?.mcpWaitingActive === true,
                runningCheckActive: binding?.runningCheckActive === true,
                visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                connectedActive: binding?.connectedActive === true,
                checkMessagesStatus: String(binding?.checkMessagesStatus || '')
            }))
        };
        this._logBatchRetry('single channel restart binding scan', { channelId, ...bindingSummary });
        if (!bindingResult.ok) {
            const error = String(bindingResult.error || 'unknown');
            const message = error === 'no_active_client'
                ? '未检测到 Cursor 注入客户端，请确认账号接管注入已生效并打开 Cursor。'
                : `CH-${channelId} 通道重试启动失败：${error}`;
            return buildBatchRetryRestartChannelFailure(channelId, 'binding_scan_failed', message, { scanError: error });
        }
        const occupiedBinding = initialProbe.effectiveWaitingActive
            ? (bindingResult.occupiedChannels || []).find((binding) => String(binding?.channelId || '').trim() === channelId && isRuntimeOccupiedChannel(binding))
            : null;
        const currentRuntimeBinding = [
            ...(Array.isArray(bindingResult.occupiedChannels) ? bindingResult.occupiedChannels : []),
            ...(Array.isArray(bindingResult.bindings) ? bindingResult.bindings : [])
        ].find((binding) => hasReusableChannelBindingRecord(binding, channelId) && isCurrentRuntimeChannelBinding(binding));
        if (occupiedBinding) {
            return buildBatchRetryRestartChannelFailure(channelId, 'bound_session_busy', `CH-${channelId} 绑定的 Cursor 会话正在运行，不能打断；等它空闲后再重试。`, { binding: occupiedBinding });
        }
        const scoreBinding = (binding) => {
            let score = 0;
            if (isCurrentRuntimeChannelBinding(binding))
                score += 300;
            if (binding?.runningCheckActive === true)
                score += 120;
            if (binding?.connectedActive === true)
                score += 80;
            if (binding?.mcpWaitingActive === true)
                score += 60;
            if (binding?.runtimeOccupancyOnly === true)
                score += 45;
            if (binding?.exists === true)
                score += 20;
            if (binding?.clientMatches === true)
                score += 15;
            if (binding?.observable === true)
                score += 10;
            if (binding?.activeComposerMatches === true)
                score += 6;
            if (String(binding?.clientId || '').trim())
                score += 5;
            if (/check_messages_scan/i.test(String(binding?.connectionReason || binding?.source || '')))
                score += 35;
            if (hasStoppedCheckMessagesEvidence(binding))
                score += 3;
            if (isReusableChannelBindingBusy(binding) && initialProbe.effectiveWaitingActive)
                score -= 100;
            return score;
        };
        const bindingCandidates = [
            ...(currentRuntimeBinding ? [currentRuntimeBinding] : []),
            ...(Array.isArray(bindingResult.occupiedChannels) ? bindingResult.occupiedChannels : []),
            ...(Array.isArray(bindingResult.bindings) ? bindingResult.bindings : []),
            ...(Array.isArray(bindingResult.allBindings) ? bindingResult.allBindings : []),
        ]
            .filter((binding) => hasReusableChannelBindingRecord(binding, channelId))
            .sort((a, b) => scoreBinding(b) - scoreBinding(a));
        const binding = bindingCandidates[0];
        if (!binding) {
            return buildBatchRetryRestartChannelFailure(channelId, 'missing_channel_binding', `CH-${channelId} 没有可复用的历史绑定，请先在 Cursor 中点选目标会话并绑定该通道。`);
        }
        if (isReusableChannelBindingBusy(binding) && initialProbe.effectiveWaitingActive) {
            return buildBatchRetryRestartChannelFailure(channelId, 'bound_session_busy', `CH-${channelId} 绑定的 Cursor 会话正在运行，不能打断；等它空闲后再重试。`, { binding });
        }
        if (binding.exists === false) {
            return buildBatchRetryRestartChannelFailure(channelId, 'stale_channel_binding', `CH-${channelId} 历史绑定已失效，请重新绑定后再重试。`, { binding });
        }
        const finalProbe = this._probeBatchRetryChannel(channelId, hasStoppedCheckMessagesEvidence(binding));
        if (!finalProbe.heartbeatOnline) {
            return buildBatchRetryRestartChannelFailure(channelId, 'mcp_not_open', `CH-${channelId} 还没打开 MCP 开关，请先在 Cursor MCP 设置里打开该通道。`, { probe: finalProbe });
        }
        if (finalProbe.effectiveWaitingActive) {
            return buildBatchRetryRestartChannelFailure(channelId, 'channel_busy_blue_light', `CH-${channelId} 正在工作/待命中，不能打断；等绿灯后再重试。`, { probe: finalProbe });
        }
        const latestQueueLength = (0, mcpServer_1.getQueueLength)(channelId);
        if (latestQueueLength > 0) {
            return buildBatchRetryRestartChannelFailure(channelId, 'queue_not_empty', `CH-${channelId} 还有 ${latestQueueLength} 条待处理消息，请处理完队列后再重试。`, { queueLength: latestQueueLength });
        }
        if (engine.isRunning() || this._batchRetryPendingRun) {
            return buildBatchRetryRestartChannelFailure(channelId, 'batch_retry_running', '批量会话重试正在运行，稍后再试，避免打断正在工作的通道。');
        }
        const runId = `br-single-${Date.now().toString(36)}-ch-${channelId}`;
        const opts = normalizeBatchRetryOpts({
            sessionCount: 1,
            reuseExisting: true,
            strictReuseExisting: true,
            useComposerBridge: true
        });
        const session = {
            sessionId: `${runId}-session`,
            channelId,
            name: `CH-${channelId}`,
            role: '',
            agentStatus: 'idle',
            composerId: String(binding.composerId || ''),
            clientId: String(binding.clientId || ''),
            online: true,
            joinPhrase: (0, mcpServer_1.prepareStartPrompt)(channelId).prompt
        };
        this._batchRetryHistoryRecordedTaskIds.clear();
        this._batchRetryNotifiedTaskIds.clear();
        this._logBatchRetry('single channel restart start', {
            runId,
            channelId,
            composerId: session.composerId,
            clientId: session.clientId,
            probe: finalProbe
        });
        const onUpdate = (status) => {
            this._recordBatchRetryHistory(status);
            this._notifyBatchRetrySuccess(status);
            try {
                webview.postMessage({ command: 'batchRetryStatus', data: status });
            }
            catch { }
        };
        const launchCallback = async (sid) => {
            this._logBatchRetry('single channel restart blocked missing launch', { runId, channelId, sid });
            return { ok: false, error: 'missing_channel_binding' };
        };
        const onlineCheck = (_sid) => false;
        try {
            await engine.startBatchRetry([session], opts, onUpdate, launchCallback, onlineCheck);
        }
        catch (e) {
            const error = e instanceof Error ? e.message : String(e);
            this._logBatchRetry('single channel restart start exception', { runId, channelId, error });
            return buildBatchRetryRestartChannelFailure(channelId, 'start_failed', `CH-${channelId} 通道重试启动失败：${error}`);
        }
        if (!engine.isRunning()) {
            return buildBatchRetryRestartChannelFailure(channelId, 'start_failed', `CH-${channelId} 通道重试启动失败：历史绑定不可用或会话未能启动`);
        }
        return {
            ok: true,
            channelId,
            message: `CH-${channelId} 已开始复用历史绑定重试`,
            sessionId: session.sessionId,
            composerId: session.composerId,
            clientId: session.clientId
        };
    }
    async _ensureBatchRetryChannelCount(targetCount) {
        const target = Math.max(1, Math.min(50, Math.floor(Number(targetCount) || 0)));
        this._logBatchRetry('ensure channel count begin', {
            target,
            current: (0, mcpServer_1.getChannelCount)()
        });
        while ((0, mcpServer_1.getChannelCount)() < target) {
            try {
                const result = await vscode.commands.executeCommand('qingtian.addChannel');
                this._logBatchRetry('ensure channel count add result', {
                    target,
                    current: (0, mcpServer_1.getChannelCount)(),
                    result
                });
                if (!result?.ok)
                    break;
            }
            catch {
                this._logBatchRetry('ensure channel count add failed', {
                    target,
                    current: (0, mcpServer_1.getChannelCount)()
                });
                break;
            }
        }
        this._logBatchRetry('ensure channel count end', {
            target,
            current: (0, mcpServer_1.getChannelCount)()
        });
    }
    async _tickBatchRetryPendingRun(run) {
        if (this._batchRetryPendingRun !== run || run.busy || run.started >= run.requested)
            return;
        run.busy = true;
        try {
            this._logBatchRetry('pending tick begin', {
                runId: run.id,
                requested: run.requested,
                started: run.started,
                pendingChannelIds: Array.from(run.pendingChannelIds),
                scheduledChannelIds: Array.from(run.scheduledChannelIds),
                reuseBindingChannelIds: Array.from(run.reuseBindingChannelIds || []),
                allowedChannelIds: Array.from(run.allowedChannelIds || []),
                targetClientId: run.targetClientId || '',
                addedChannelIds: Array.from(run.addedChannelIds)
            });
            const occupiedChannelIds = new Set();
            const occupiedBindingByChannel = new Map();
            const boundBindingByChannel = new Map();
            const releasedChannelIds = new Set();
            for (const [channelId, binding] of run.reuseBindingByChannel || []) {
                if (hasReusableChannelBindingRecord(binding, channelId) && !boundBindingByChannel.has(channelId)) {
                    boundBindingByChannel.set(channelId, binding);
                }
            }
            const engine = this._batchRetryEngine;
            if (engine) {
                try {
                    const bindingResult = await engine.listChannelBindings(2500, run.targetClientId ? {
                        targetClientId: run.targetClientId,
                        targetClientSeenAfter: run.targetClientSeenAfter
                    } : undefined);
                    this._logBatchRetry('pending tick channel binding scan', {
                        runId: run.id,
                        ok: bindingResult.ok,
                        bindings: (bindingResult.bindings || []).map((binding) => ({
                            channelId: String(binding?.channelId || ''),
                            composerId: String(binding?.composerId || ''),
                            clientId: String(binding?.clientId || ''),
                            clientMatches: binding?.clientMatches === true,
                            exists: binding?.exists === true,
                            observable: binding?.observable === true,
                            activeComposerMatches: binding?.activeComposerMatches === true,
                            runningCheckActive: binding?.runningCheckActive === true,
                            mcpWaitingActive: binding?.mcpWaitingActive === true,
                            mcpWaitingUpdatedAt: Number(binding?.mcpWaitingUpdatedAt || 0) || null,
                            visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                            connectedActive: binding?.connectedActive === true,
                            connectionReason: String(binding?.connectionReason || ''),
                            checkMessagesStatus: String(binding?.checkMessagesStatus || ''),
                            bottomStopButton: binding?.bottomStopButton === true,
                            bottomMicButton: binding?.bottomMicButton === true
                        })),
                        allBindings: (bindingResult.allBindings || []).map((binding) => ({
                            channelId: String(binding?.channelId || ''),
                            composerId: String(binding?.composerId || ''),
                            clientId: String(binding?.clientId || ''),
                            clientMatches: binding?.clientMatches === true,
                            exists: binding?.exists === true,
                            observable: binding?.observable === true,
                            activeComposerMatches: binding?.activeComposerMatches === true,
                            runningCheckActive: binding?.runningCheckActive === true,
                            mcpWaitingActive: binding?.mcpWaitingActive === true,
                            mcpWaitingUpdatedAt: Number(binding?.mcpWaitingUpdatedAt || 0) || null,
                            visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                            connectedActive: binding?.connectedActive === true,
                            connectionReason: String(binding?.connectionReason || ''),
                            checkMessagesStatus: String(binding?.checkMessagesStatus || ''),
                            bottomStopButton: binding?.bottomStopButton === true,
                            bottomMicButton: binding?.bottomMicButton === true,
                            stoppedEvidence: hasStoppedCheckMessagesEvidence(binding)
                        })),
                        occupiedChannels: (bindingResult.occupiedChannels || []).map((binding) => ({
                            channelId: String(binding?.channelId || ''),
                            composerId: String(binding?.composerId || ''),
                            clientId: String(binding?.clientId || ''),
                            clientMatches: binding?.clientMatches === true,
                            exists: binding?.exists === true,
                            observable: binding?.observable === true,
                            activeComposerMatches: binding?.activeComposerMatches === true,
                            runningCheckActive: binding?.runningCheckActive === true,
                            mcpWaitingActive: binding?.mcpWaitingActive === true,
                            mcpWaitingUpdatedAt: Number(binding?.mcpWaitingUpdatedAt || 0) || null,
                            visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                            connectedActive: binding?.connectedActive === true,
                            runtimeOccupancyOnly: binding?.runtimeOccupancyOnly === true,
                            connectionReason: String(binding?.connectionReason || ''),
                            checkMessagesStatus: String(binding?.checkMessagesStatus || ''),
                            bottomStopButton: binding?.bottomStopButton === true,
                            bottomMicButton: binding?.bottomMicButton === true
                        }))
                    });
                    if (bindingResult.ok) {
                        for (const binding of bindingResult.occupiedChannels || []) {
                            const channelId = String(binding.channelId || '').trim();
                            if (!channelId || !isRuntimeOccupiedChannel(binding))
                                continue;
                            occupiedChannelIds.add(channelId);
                            if (!occupiedBindingByChannel.has(channelId)) {
                                occupiedBindingByChannel.set(channelId, binding);
                            }
                        }
                        for (const binding of bindingResult.allBindings || []) {
                            const channelId = String(binding.channelId || '').trim();
                            if (channelId && hasStoppedCheckMessagesEvidence(binding)) {
                                releasedChannelIds.add(channelId);
                            }
                            if (hasReusableChannelBindingRecord(binding, channelId) &&
                                !boundBindingByChannel.has(channelId)) {
                                boundBindingByChannel.set(channelId, binding);
                            }
                            if (hasReusableChannelBindingRecord(binding, channelId) &&
                                binding.mcpWaitingActive === true) {
                                occupiedChannelIds.add(channelId);
                                if (!occupiedBindingByChannel.has(channelId)) {
                                    occupiedBindingByChannel.set(channelId, binding);
                                }
                            }
                        }
                        for (const binding of bindingResult.bindings || []) {
                            const channelId = String(binding.channelId || '').trim();
                            const composerId = String(binding.composerId || '').trim();
                            if (!channelId || !composerId)
                                continue;
                            const occupiedByBinding = !!(binding.exists !== false &&
                                binding.mcpWaitingActive === true);
                            if (!occupiedByBinding)
                                continue;
                            occupiedChannelIds.add(channelId);
                            if (!occupiedBindingByChannel.has(channelId)) {
                                occupiedBindingByChannel.set(channelId, binding);
                            }
                            if (!boundBindingByChannel.has(channelId)) {
                                boundBindingByChannel.set(channelId, binding);
                            }
                        }
                    }
                }
                catch (e) {
                    this._logBatchRetry('pending tick channel binding scan failed', {
                        runId: run.id,
                        error: e instanceof Error ? e.message : String(e)
                    });
                }
            }
            const isReady = (channelId) => {
                const probe = this._probeBatchRetryChannel(channelId, releasedChannelIds.has(String(channelId || '').trim()));
                const occupied = occupiedChannelIds.has(channelId);
                const waitsForReusableBinding = run.reuseBindingChannelIds?.has(channelId) === true;
                const boundBinding = boundBindingByChannel.get(channelId);
                const hasBoundBinding = hasReusableChannelBindingRecord(boundBinding, channelId);
                const boundReadyForSubmit = waitsForReusableBinding
                    && hasBoundBinding
                    && probe.heartbeatOnline === true
                    && !isReusableChannelBindingBusy(boundBinding);
                const allowed = !run.allowedChannelIds || run.allowedChannelIds.has(channelId);
                const scheduled = run.scheduledChannelIds.has(channelId);
                const ready = allowed && !scheduled && (waitsForReusableBinding
                    ? (occupied || boundReadyForSubmit)
                    : (!occupied && probe.heartbeatOnline));
                this._logBatchRetry('pending tick channel readiness', {
                    runId: run.id,
                    channelId,
                    ready,
                    occupied,
                    boundReadyForSubmit,
                    hasBoundBinding,
                    boundComposerId: boundBinding ? String(boundBinding.composerId || '') : '',
                    boundClientId: boundBinding ? String(boundBinding.clientId || '') : '',
                    boundClientMatches: boundBinding?.clientMatches === true,
                    boundExists: boundBinding?.exists === true,
                    boundBusy: isReusableChannelBindingBusy(boundBinding),
                    waitsForReusableBinding,
                    allowed,
                    scheduled,
                    probe
                });
                return ready;
            };
            const consumePendingSlot = (channelId) => {
                if (run.pendingChannelIds.has(channelId)) {
                    run.pendingChannelIds.delete(channelId);
                    return channelId;
                }
                const fallback = Array.from(run.pendingChannelIds)[0];
                if (!fallback)
                    return null;
                run.pendingChannelIds.delete(fallback);
                return fallback;
            };
            const readySessions = [];
            const readyReservations = [];
            const reservedThisTick = new Set();
            const tryAddReady = (channelId) => {
                if (readySessions.length + run.started >= run.requested)
                    return;
                if (reservedThisTick.has(channelId))
                    return;
                const ready = isReady(channelId);
                if (!ready)
                    return;
                const slotId = consumePendingSlot(channelId);
                if (!slotId)
                    return;
                const binding = run.reuseBindingChannelIds?.has(channelId) === true
                    ? (occupiedBindingByChannel.get(channelId) || boundBindingByChannel.get(channelId) || run.reuseBindingByChannel?.get(channelId))
                    : undefined;
                const session = run.makeSession(channelId, binding);
                readySessions.push(session);
                readyReservations.push({ channelId, slotId });
                reservedThisTick.add(channelId);
                this._logBatchRetry('pending tick reserve ready channel', {
                    runId: run.id,
                    channelId,
                    slotId,
                    sessionId: session.sessionId
                });
            };
            for (const channelId of Array.from(run.pendingChannelIds)) {
                tryAddReady(channelId);
            }
            const configuredCount = Math.max(0, (0, mcpServer_1.getChannelCount)());
            for (let i = 1; i <= configuredCount && readySessions.length + run.started < run.requested; i++) {
                tryAddReady(String(i));
            }
            if (readySessions.length > 0) {
                const detectedWait = this._makeBatchRetryChannelWaitStatus(run, 'detected');
                const appendingChannelIds = readyReservations.map(item => item.channelId);
                detectedWait.active = true;
                detectedWait.pending = Math.max(detectedWait.pending, appendingChannelIds.length);
                detectedWait.waitingChannelIds = Array.from(new Set([
                    ...detectedWait.waitingChannelIds,
                    ...appendingChannelIds
                ])).sort((a, b) => Number(a) - Number(b));
                detectedWait.message = '检测到已打开 MCP，正在自动添加剩余任务';
                this._pushBatchRetryPendingStatus(run, detectedWait);
                this._logBatchRetry('pending tick append sessions begin', {
                    runId: run.id,
                    sessions: readySessions.map(session => ({
                        sessionId: session.sessionId,
                        channelId: session.channelId,
                        name: session.name
                    })),
                    reservations: readyReservations
                });
                const appended = await run.append(readySessions);
                const appendedCount = Math.max(0, Math.min(readySessions.length, appended));
                run.started += appendedCount;
                for (const reservation of readyReservations.slice(0, appendedCount)) {
                    run.addedChannelIds.add(reservation.channelId);
                    run.scheduledChannelIds.add(reservation.channelId);
                }
                for (const reservation of readyReservations.slice(appendedCount)) {
                    run.pendingChannelIds.add(reservation.slotId);
                }
                this._logBatchRetry('pending tick append sessions result', {
                    runId: run.id,
                    appended,
                    appendedCount,
                    requested: run.requested,
                    started: run.started,
                    pendingChannelIds: Array.from(run.pendingChannelIds),
                    scheduledChannelIds: Array.from(run.scheduledChannelIds),
                    addedChannelIds: Array.from(run.addedChannelIds)
                });
            }
            if (run.started >= run.requested) {
                const doneWait = this._makeBatchRetryChannelWaitStatus(run, 'detected');
                doneWait.active = false;
                doneWait.pending = 0;
                doneWait.message = '剩余任务已自动启动';
                this._batchRetryEngine?.setChannelWait(doneWait);
                this._clearBatchRetryPendingRun(false);
                setTimeout(() => {
                    if (this._batchRetryPendingRun !== run) {
                        this._batchRetryEngine?.setChannelWait(null);
                    }
                }, 1800);
            }
            else {
                this._logBatchRetry('pending tick still waiting', {
                    runId: run.id,
                    requested: run.requested,
                    started: run.started,
                    pendingChannelIds: Array.from(run.pendingChannelIds)
                });
                this._pushBatchRetryPendingStatus(run);
            }
        }
        finally {
            run.busy = false;
        }
    }
    _startBatchRetryPendingRun(run) {
        this._clearBatchRetryPendingRun(false);
        this._batchRetryPendingRun = run;
        this._logBatchRetry('pending run started', {
            runId: run.id,
            requested: run.requested,
            started: run.started,
            pendingChannelIds: Array.from(run.pendingChannelIds),
            scheduledChannelIds: Array.from(run.scheduledChannelIds),
            reuseBindingChannelIds: Array.from(run.reuseBindingChannelIds || []),
            allowedChannelIds: Array.from(run.allowedChannelIds || []),
            addedChannelIds: Array.from(run.addedChannelIds)
        });
        this._pushBatchRetryPendingStatus(run);
        run.timer = setInterval(() => {
            void this._tickBatchRetryPendingRun(run);
        }, 2000);
        void this._tickBatchRetryPendingRun(run);
    }
    _readBatchRetryNetworkState() {
        const cfg = vscode.workspace.getConfiguration();
        const rawDisableHttp2 = cfg.get(CURSOR_DISABLE_HTTP2_KEY);
        const rawDisableHttp1SSE = cfg.get(CURSOR_DISABLE_HTTP1_SSE_KEY);
        const disableHttp2 = rawDisableHttp2 === true;
        const disableHttp1SSE = rawDisableHttp1SSE === true;
        let value = 'unknown';
        if (!disableHttp2 && !disableHttp1SSE)
            value = '2.0';
        else if (disableHttp2 && !disableHttp1SSE)
            value = '1.1';
        else if (disableHttp2 && disableHttp1SSE)
            value = '1.0';
        return {
            value,
            label: value === 'unknown' ? '未识别' : value,
            disableHttp2,
            disableHttp1SSE,
            raw: {
                disableHttp2: rawDisableHttp2,
                disableHttp1SSE: rawDisableHttp1SSE
            }
        };
    }
    async _setBatchRetryNetworkType(value) {
        const normalized = String(value || '').trim();
        const cfg = vscode.workspace.getConfiguration();
        try {
            if (normalized === '2.0') {
                await cfg.update(CURSOR_DISABLE_HTTP2_KEY, false, vscode.ConfigurationTarget.Global);
                await cfg.update(CURSOR_DISABLE_HTTP1_SSE_KEY, false, vscode.ConfigurationTarget.Global);
            }
            else if (normalized === '1.1') {
                await cfg.update(CURSOR_DISABLE_HTTP2_KEY, true, vscode.ConfigurationTarget.Global);
                await cfg.update(CURSOR_DISABLE_HTTP1_SSE_KEY, false, vscode.ConfigurationTarget.Global);
            }
            else if (normalized === '1.0') {
                await cfg.update(CURSOR_DISABLE_HTTP2_KEY, true, vscode.ConfigurationTarget.Global);
                await cfg.update(CURSOR_DISABLE_HTTP1_SSE_KEY, true, vscode.ConfigurationTarget.Global);
            }
            else {
                return { ok: false, state: this._readBatchRetryNetworkState(), error: 'unsupported_network_type' };
            }
            return { ok: true, state: this._readBatchRetryNetworkState() };
        }
        catch (e) {
            return {
                ok: false,
                state: this._readBatchRetryNetworkState(),
                error: e instanceof Error ? e.message : String(e)
            };
        }
    }
    postBatchRetryNetworkState() {
        try {
            this._view?.webview.postMessage({
                command: 'batchRetryNetworkState',
                data: this._readBatchRetryNetworkState()
            });
        }
        catch { }
    }
    _getBatchRetryHistory() {
        try {
            const ctx = require('./extension').getExtensionContext?.();
            const raw = ctx?.globalState?.get(BATCH_RETRY_HISTORY_KEY) || [];
            if (!Array.isArray(raw))
                return [];
            return raw
                .map((item) => ({
                id: String(item?.id || ''),
                taskId: String(item?.taskId || ''),
                batchId: String(item?.batchId || ''),
                sessionId: String(item?.sessionId || ''),
                channelId: String(item?.channelId || ''),
                name: String(item?.name || ''),
                windowId: String(item?.windowId || ''),
                composerId: String(item?.composerId || ''),
                mode: item?.mode === 'silent' || item?.mode === 'visible' ? item.mode : '',
                status: String(item?.status || ''),
                success: !!item?.success,
                retryCount: Math.max(0, Number(item?.retryCount) || 0),
                startedAt: Number(item?.startedAt) || 0,
                finishedAt: Number(item?.finishedAt) || 0,
                durationMs: Math.max(0, Number(item?.durationMs) || 0),
                error: String(item?.error || '')
            }))
                .filter((item) => item.id && item.taskId && item.finishedAt > 0)
                .slice(0, BATCH_RETRY_HISTORY_MAX);
        }
        catch {
            return [];
        }
    }
    _saveBatchRetryHistory(items) {
        try {
            const ctx = require('./extension').getExtensionContext?.();
            ctx?.globalState?.update(BATCH_RETRY_HISTORY_KEY, items.slice(0, BATCH_RETRY_HISTORY_MAX));
        }
        catch { }
    }
    _recordBatchRetryHistory(status) {
        const tasks = Array.isArray(status.tasks) ? status.tasks : [];
        const terminalTasks = tasks.filter(task => {
            const state = String(task.status || '');
            return state === 'done' || state === 'failed' || state === 'stopped';
        });
        if (terminalTasks.length === 0)
            return;
        const history = this._getBatchRetryHistory();
        const existingTaskIds = new Set(history.map(item => item.taskId));
        const newItems = [];
        for (const task of terminalTasks) {
            const taskId = String(task.id || '');
            if (!taskId)
                continue;
            if (this._batchRetryHistoryRecordedTaskIds.has(taskId) || existingTaskIds.has(taskId))
                continue;
            const finishedAt = Number(task.finishedAt) || Date.now();
            const startedAt = Number(task.startedAt || task.sessionBaseline?.startedAt) || finishedAt;
            const durationMs = Math.max(0, finishedAt - startedAt);
            const item = {
                id: `${taskId}-${finishedAt}`,
                taskId,
                batchId: String(task.batchId || ''),
                sessionId: String(task.sessionId || ''),
                channelId: String(task.channelId || ''),
                name: String(task.title || task.sessionId || ''),
                windowId: String(task.clientId || ''),
                composerId: String(task.composerId || ''),
                mode: task.mode === 'silent' || task.mode === 'visible' ? task.mode : '',
                status: String(task.status || ''),
                success: task.status === 'done',
                retryCount: Math.max(0, Number(task.retryCount) || 0),
                startedAt,
                finishedAt,
                durationMs,
                error: String(task.error || task.evidence || '')
            };
            this._batchRetryHistoryRecordedTaskIds.add(taskId);
            newItems.push(item);
        }
        if (newItems.length === 0)
            return;
        this._saveBatchRetryHistory([...newItems.reverse(), ...history]);
    }
    _notifyBatchRetrySuccess(status) {
        const notifyEnabled = vscode.workspace.getConfiguration('qingtian').get('notifyOnReply', false);
        if (!notifyEnabled)
            return;
        const tasks = Array.isArray(status.tasks) ? status.tasks : [];
        for (const task of tasks) {
            if (task.status !== 'done')
                continue;
            const taskId = String(task.id || task.sessionId || '').trim();
            if (!taskId || this._batchRetryNotifiedTaskIds.has(taskId))
                continue;
            this._batchRetryNotifiedTaskIds.add(taskId);
            const channelId = String(task.channelId || '').trim();
            const sessionName = String(task.title || task.sessionId || '').trim();
            const suffix = channelId ? `CH-${channelId}` : (sessionName || '会话');
            const message = `批量会话重试成功：${suffix}`;
            vscode.window.showInformationMessage(uiText(message));
            trySystemNotify(uiText('晴天无限MCP'), uiText(message));
        }
    }
    _getBatchRetryPreflightMessage() {
        const injectionStatus = (0, seamlessSwitch_1.getSeamlessInjectionStatus)();
        if (injectionStatus.injected && !injectionStatus.current) {
            return '账号接管注入脚本版本过旧，请重新注入并完整重启 Cursor，再启动批量会话重试。';
        }
        const runtimeStatus = (0, seamlessSwitch_1.getSeamlessRuntimeStatus)();
        if (injectionStatus.current && !runtimeStatus.runtimeLoaded) {
            return '账号接管注入已更新，但当前 Cursor 窗口尚未加载新脚本。请完整退出并重启 Cursor，再启动批量会话重试。';
        }
        if (process.platform === 'darwin') {
            return '批量会话重试需要先启用账号接管注入并重启 Cursor。Mac 如仍不可用，请检查 Node.js 是否安装到 /opt/homebrew/bin 或 /usr/local/bin、Cursor.app 是否允许写入，以及系统设置中的辅助功能是否已授权 Cursor。';
        }
        return '请先启用账号接管注入并重启 Cursor，再启动批量会话重试。';
    }
    _getBatchRetryPlatformBlocker() {
        if (process.platform !== 'darwin') {
            return '';
        }
        const nodeCandidates = ['/opt/homebrew/bin/node', '/usr/local/bin/node'];
        const hasNode = nodeCandidates.some(p => fs.existsSync(p)) || (() => {
            try {
                cp.execFileSync('/usr/bin/which', ['node'], { timeout: 3000, stdio: ['ignore', 'pipe', 'ignore'] });
                return true;
            }
            catch {
                return false;
            }
        })();
        if (!hasNode) {
            return 'Mac 未检测到 Node.js。请先安装 Node.js v18+，建议 Homebrew 安装后确认 /opt/homebrew/bin/node 或 /usr/local/bin/node 存在，再重载 Cursor。';
        }
        return '';
    }
    async _ensureActivated(actionLabel) {
        return true;
    }
    resolveWebviewView(webviewView, _context, _token) {
        this._view = webviewView;
        try {
            fs.mkdirSync(PASTE_IMAGE_TMP_DIR, { recursive: true });
        }
        catch { }
        webviewView.webview.options = {
            enableScripts: true,
            localResourceRoots: [this._extensionUri, vscode.Uri.file(PASTE_IMAGE_TMP_DIR)]
        };
        webviewView.webview.html = this._getHtmlContent(webviewView.webview);
        // === Patch: Force activated=true in all messages sent to webview ===
        const _origWebviewPostMessage = webviewView.webview.postMessage.bind(webviewView.webview);
        webviewView.webview.postMessage = function(msg) {
            if (msg && typeof msg === 'object') {
                if (msg.command === 'licenseStatus') {
                    msg.activated = true;
                    msg.remainingDays = 999;
                    if (msg.message && (msg.message.includes('授权') || msg.message.includes('付款') || msg.message.includes('续费'))) {
                        msg.message = '';
                    }
                }
                if (msg.command === 'status' && msg.data) {
                    if (msg.data.activated === false) msg.data.activated = true;
                    if (msg.data.licenseStatus) msg.data.licenseStatus.activated = true;
                }
            }
            return _origWebviewPostMessage(msg);
        };
        try {
            const { setActivationInvalidHandler, getPaymentInfo, getLicenseCountdownStatus, hasConsumedTrial } = require('./activation');
            setActivationInvalidHandler((message) => {
                try {
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: false,
                        message: message || '授权已到期，请完成付款',
                        payment: this._paymentPayload(webviewView),
                        license: getLicenseCountdownStatus(),
                        trialConsumed: hasConsumedTrial()
                    });
                    this.focusPanel();
                    vscode.window.showWarningMessage(uiText(String(message || '试用/订阅已到期，请完成付款后续费')));
                }
                catch (e) {
                    console.warn('[QingTian] 推送到期门禁失败:', e);
                }
            });
        }
        catch (e) {
            console.warn('[QingTian] 注册到期回调失败:', e);
        }
        // 初始推送一次快捷指令清单，并订阅后续变更（来自浏览器侧等其他入口）。
        try {
            webviewView.webview.postMessage({ command: 'quickCommands', data: (0, quickCommands_1.getQuickCommands)() });
        }
        catch { }
        this.postBatchRetryNetworkState();
        if (this._quickCommandsUnsubscribe) {
            try {
                this._quickCommandsUnsubscribe();
            }
            catch { }
        }
        this._quickCommandsUnsubscribe = (0, quickCommands_1.subscribeQuickCommands)((list) => {
            try {
                webviewView.webview.postMessage({ command: 'quickCommands', data: list });
            }
            catch { }
        });
        webviewView.onDidDispose(() => {
            if (this._quickCommandsUnsubscribe) {
                try {
                    this._quickCommandsUnsubscribe();
                }
                catch { }
                this._quickCommandsUnsubscribe = null;
            }
        });
        webviewView.webview.onDidReceiveMessage(async (msg) => {
            switch (msg.command) {
                case 'submit': {
                    if (!await this._ensureActivated('发送消息')) {
                        webviewView.webview.postMessage({
                            command: 'messageSent',
                            success: false,
                            error: '授权已失效'
                        });
                        break;
                    }
                    const channelId = msg.channelId || '1';
                    const mcpPrefix = buildMcpCallPrefix(msg?.data?.mcp_servers);
                    if (mcpPrefix && msg.data?.user_input) {
                        msg.data.user_input = mcpPrefix + String(msg.data.user_input || '');
                    }
                    const result = (0, mcpServer_1.sendUserMessage)(channelId, msg.data, 'plugin');
                    if (result.ok) {
                        webviewView.webview.postMessage({
                            command: 'messageSent',
                            success: true,
                            channelId,
                            text: msg.data?.user_input || ''
                        });
                    }
                    else {
                        const errMsg = result.error || '发送消息失败';
                        vscode.window.showErrorMessage(uiText(errMsg));
                        webviewView.webview.postMessage({ command: 'messageSent', success: false, error: errMsg });
                    }
                    break;
                }
                case 'copyStartPrompt': {
                    if (!await this._ensureActivated('复制开头语')) {
                        break;
                    }
                    const chId = msg.channelId || '1';
                    let force = msg.force === true;
                    const gate = mcpServer_1.assertCanStartNewSession
                        ? (0, mcpServer_1.assertCanStartNewSession)(chId, { force })
                        : { ok: true };
                    if (!gate.ok) {
                        const pick = await vscode.window.showWarningMessage(
                            gate.message || '当前通道仍在保活，重新开场会消耗新额度。',
                            { modal: true },
                            '拉回循环',
                            '强制新开场（耗额度）',
                            '取消'
                        );
                        if (pick === '拉回循环') {
                            const resumed = await vscode.commands.executeCommand('qingtian.resumeLoop', chId);
                            vscode.window.showInformationMessage(uiText(resumed?.message || '已尝试拉回循环'));
                            webviewView.webview.postMessage({ command: 'resumeLoopResult', ...(resumed || {}) });
                            break;
                        }
                        if (pick !== '强制新开场（耗额度）') {
                            break;
                        }
                        force = true;
                    }
                    const prepared = (0, mcpServer_1.prepareStartPrompt)(chId);
                    const prompt = prepared.prompt;
                    try {
                        await vscode.env.clipboard.writeText(prompt);
                        vscode.window.showInformationMessage(uiText(`通道 ${chId} 开头语已复制到剪贴板`));
                        webviewView.webview.postMessage({
                            command: 'promptCopied',
                            channelId: chId,
                            text: prompt,
                            forced: force
                        });
                    }
                    catch (e) {
                        const errMsg = '复制开场失败：' + (e instanceof Error ? e.message : String(e));
                        vscode.window.showErrorMessage(uiText(errMsg));
                        webviewView.webview.postMessage({
                            command: 'promptCopyFailed',
                            error: errMsg
                        });
                    }
                    break;
                }
                case 'sendStartPrompt': {
                    if (!await this._ensureActivated('一键发送开场白')) {
                        break;
                    }
                    const chId = msg.channelId || '1';
                    let force = msg.force === true;
                    const gate = mcpServer_1.assertCanStartNewSession
                        ? (0, mcpServer_1.assertCanStartNewSession)(chId, { force })
                        : { ok: true };
                    if (!gate.ok) {
                        const pick = await vscode.window.showWarningMessage(
                            gate.message || '当前通道仍在保活，重新开场会消耗新额度。',
                            { modal: true },
                            '拉回循环',
                            '强制新开场（耗额度）',
                            '取消'
                        );
                        if (pick === '拉回循环') {
                            const resumed = await vscode.commands.executeCommand('qingtian.resumeLoop', chId);
                            vscode.window.showInformationMessage(uiText(resumed?.message || '已尝试拉回循环'));
                            webviewView.webview.postMessage({ command: 'resumeLoopResult', ...(resumed || {}) });
                            break;
                        }
                        if (pick !== '强制新开场（耗额度）') {
                            webviewView.webview.postMessage({
                                command: 'startPromptSendResult',
                                ok: false,
                                blocked: true,
                                channelId: chId,
                                message: '已取消新开场，以保护额度'
                            });
                            break;
                        }
                        force = true;
                    }
                    const result = await vscode.commands.executeCommand('qingtian.sendStartPrompt', chId, {
                        openComposer: msg.openComposer !== false,
                        targetMode: msg.targetMode === 'bound' ? 'bound' : 'active',
                        force
                    });
                    webviewView.webview.postMessage({
                        command: 'startPromptSendResult',
                        ok: !!result?.ok,
                        blocked: !!result?.blocked,
                        channelId: chId,
                        message: result?.message || ''
                    });
                    break;
                }
                case 'resumeLoop': {
                    if (!await this._ensureActivated('拉回循环')) {
                        break;
                    }
                    const chId = msg.channelId || '1';
                    const resumed = await vscode.commands.executeCommand('qingtian.resumeLoop', chId);
                    vscode.window.showInformationMessage(uiText(resumed?.message || '已尝试拉回循环'));
                    webviewView.webview.postMessage({ command: 'resumeLoopResult', ...(resumed || {}), channelId: chId });
                    break;
                }
                case 'stopChannelTurn': {
                    if (!await this._ensureActivated('停止当前')) {
                        break;
                    }
                    const chId = msg.channelId || '1';
                    const stopped = await vscode.commands.executeCommand('qingtian.stopChannelTurn', chId);
                    vscode.window.showInformationMessage(uiText(stopped?.message || `已停止 CH-${chId}`));
                    webviewView.webview.postMessage({ command: 'stopChannelTurnResult', ...(stopped || {}), channelId: chId });
                    // refresh status so banner/online update
                    try {
                        const { getLicenseCountdownStatus, isActivated, hasConsumedTrial } = require('./activation');
                        webviewView.webview.postMessage({
                            command: 'licenseStatus',
                            activated: isActivated(),
                            payment: this._paymentPayload(webviewView),
                            license: getLicenseCountdownStatus(),
                            trialConsumed: hasConsumedTrial()
                        });
                    } catch { }
                    break;
                }
                case 'restoreRecoveryContext': {
                    const sourceChannelId = String(msg.sourceChannelId || msg.channelId || '1');
                    const targetChannelId = String(msg.targetChannelId || sourceChannelId);
                    const recoveryScope = msg.scope === 'workspace' || msg.scope === 'group' ? msg.scope : 'channel';
                    const recoveryMaxChars = Number(msg.maxChars || 12000);
                    const workspacePath = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || '';
                    // Cursor UI 同源优先：agent-transcripts jsonl → vscdb 气泡 → 插件档案（enqueue 内兜底）
                    let nativeConversationText = '';
                    let recoverySource = 'archive';
                    if (recoveryScope === 'channel') {
                        try {
                            const composerId = await this._resolveChannelComposerId(sourceChannelId);
                            if (composerId) {
                                const resolved = await (0, agentTranscriptIo_1.resolveRecoveryTextForComposer)(composerId, {
                                    maxChars: recoveryMaxChars,
                                    workspacePath
                                });
                                if (resolved.text) {
                                    nativeConversationText = resolved.text;
                                    recoverySource = resolved.source;
                                    this._logBatchRetry('recovery context resolved', {
                                        sourceChannelId,
                                        composerId,
                                        recoverySource,
                                        filePath: resolved.filePath || '',
                                        lineCount: resolved.lineCount || 0,
                                        chars: nativeConversationText.length
                                    });
                                }
                            }
                        }
                        catch (e) {
                            this._logBatchRetry('recovery context resolve failed', {
                                sourceChannelId,
                                error: e instanceof Error ? e.message : String(e)
                            });
                        }
                    }
                    const result = (0, mcpServer_1.enqueueRecoveryContext)(targetChannelId, {
                        sourceChannelId,
                        targetChannelId,
                        scope: recoveryScope,
                        groupId: String(msg.groupId || ''),
                        depth: msg.depth === 'fast' || msg.depth === 'deep' ? msg.depth : 'standard',
                        maxChars: recoveryMaxChars,
                        nativeConversationText,
                        recoveryTextSource: recoverySource
                    });
                    if (result.ok) {
                        const info = sourceChannelId === targetChannelId
                            ? `CH-${targetChannelId} 恢复上下文已投递；当前或下一个绑定该通道的 Cursor 窗口会自动接手。`
                            : `已将 CH-${sourceChannelId} 的上下文转移到 CH-${targetChannelId}；目标通道窗口会自动接手。`;
                        vscode.window.showInformationMessage(uiText(info));
                    }
                    else {
                        vscode.window.showErrorMessage(uiText('自动恢复上下文失败：' + (result.error || '未知错误')));
                    }
                    webviewView.webview.postMessage({
                        command: 'recoveryRestoreResult',
                        ok: result.ok,
                        sourceChannelId,
                        targetChannelId,
                        entryCount: result.packet?.entryCount || 0,
                        message: result.error || ''
                    });
                    break;
                }
                case 'getStatus': {
                    try {
                        const { isActivated, getLicenseCountdownStatus, getPaymentInfo, hasConsumedTrial } = require('./activation');
                        webviewView.webview.postMessage({
                            command: 'licenseStatus',
                            activated: isActivated(),
                            payment: this._paymentPayload(webviewView),
                            license: getLicenseCountdownStatus(),
                            trialConsumed: hasConsumedTrial()
                        });
                        // 新手引导已关闭：不再自动弹出使用提示
                        const status = (0, statusPayload_1.buildStatusPayload)(this._getDisplayVersion());
                        webviewView.webview.postMessage({
                            command: 'status',
                            data: {
                                ...status,
                                license: getLicenseCountdownStatus()
                            }
                        });
                    }
                    catch { }
                    break;
                }
                case 'getMcpServers': {
                    webviewView.webview.postMessage({
                        command: 'mcpServers',
                        data: readWorkspaceMcpServers()
                    });
                    break;
                }
                case 'onboardingDone': {
                    // 用户完成/跳过新手引导 → 全局标记已看 + 清除续接标记，之后不再自动弹出
                    try {
                        const ctx = require('./extension').getExtensionContext?.();
                        ctx?.globalState?.update('qingtian.onboardingSeen', true);
                        ctx?.globalState?.update('qingtian.onboardingResume', '');
                    }
                    catch { }
                    break;
                }
                case 'onboardingPersistResume': {
                    // 突破账单：未注入时引导用户去注入，持久化续接标记，重启后自动继续批量重试引导
                    try {
                        const ctx = require('./extension').getExtensionContext?.();
                        ctx?.globalState?.update('qingtian.onboardingResume', String(msg?.value || 'billing'));
                    }
                    catch { }
                    break;
                }
                case 'updatePluginSettings': {
                    const next = (0, mcpServer_1.updatePluginSettings)(msg?.data || {});
                    try {
                        const cfg = vscode.workspace.getConfiguration('qingtian');
                        (0, mcpServer_1.writeRuntimeConfig)({
                            keepaliveEnabled: cfg.get('keepaliveEnabled', true),
                            keepaliveMinutes: Math.max(1, Math.min(120, cfg.get('keepaliveMinutes', 45))),
                            bridgeEnabled: cfg.get('bridgeEnabled', false),
                            bridgeChannel: Math.max(1, Math.floor(Number(cfg.get('bridgeChannel', 1)) || 1)),
                            bridgeBotToken: cfg.get('bridgeBotToken', ''),
                            bridgeUseProxy: (0, mcpServer_1.getBridgeUseProxy)(),
                            agentTeamEnabled: next.agentTeamEnabled === true
                        });
                    }
                    catch (e) {
                        console.warn('[QingTian] 同步到 Agent 运行时开关失败', e);
                    }
                    webviewView.webview.postMessage({
                        command: 'pluginSettings',
                        data: {
                            settings: next
                        }
                    });
                    break;
                }
                case 'updateKeepaliveSettings': {
                    const cfg = vscode.workspace.getConfiguration('qingtian');
                    const patch = msg?.data || {};
                    if (Object.prototype.hasOwnProperty.call(patch, 'keepaliveEnabled')) {
                        await cfg.update('keepaliveEnabled', Boolean(patch.keepaliveEnabled), vscode.ConfigurationTarget.Global);
                    }
                    if (Object.prototype.hasOwnProperty.call(patch, 'keepaliveMinutes')) {
                        const mins = Math.max(1, Math.min(120, Number(patch.keepaliveMinutes) || 45));
                        await cfg.update('keepaliveMinutes', mins, vscode.ConfigurationTarget.Global);
                    }
                    const updated = {
                        keepaliveEnabled: cfg.get('keepaliveEnabled', true),
                        keepaliveMinutes: cfg.get('keepaliveMinutes', 45)
                    };
                    webviewView.webview.postMessage({
                        command: 'keepaliveSettings',
                        data: updated
                    });
                    break;
                }
                case 'updateNotifySettings': {
                    const cfg2 = vscode.workspace.getConfiguration('qingtian');
                    const notifyPatch = msg?.data || {};
                    if (Object.prototype.hasOwnProperty.call(notifyPatch, 'notifyOnReply')) {
                        const val = Boolean(notifyPatch.notifyOnReply);
                        await cfg2.update('notifyOnReply', val, vscode.ConfigurationTarget.Global);
                        if (val) {
                            vscode.window.showInformationMessage(uiText('回复通知已开启，AI 处理完毕时将弹出提醒'));
                            if (process.platform === 'win32') {
                                try {
                                    const nn = require('node-notifier');
                                    nn.notify({ title: 'SlashSubs', message: '通知功能已开启，AI 回复时您将收到桌面提醒。', sound: true });
                                }
                                catch (e) {
                                    console.error('[QingTian] 测试通知发送失败', e);
                                }
                            }
                        }
                    }
                    webviewView.webview.postMessage({
                        command: 'notifySettings',
                        data: { notifyOnReply: cfg2.get('notifyOnReply', false) }
                    });
                    break;
                }
                case 'updateBridgeSettings': {
                    const cfg3 = vscode.workspace.getConfiguration('qingtian');
                    const bridgePatch = msg?.data || {};
                    if (Object.prototype.hasOwnProperty.call(bridgePatch, 'bridgeEnabled')) {
                        await cfg3.update('bridgeEnabled', Boolean(bridgePatch.bridgeEnabled), vscode.ConfigurationTarget.Global);
                    }
                    if (Object.prototype.hasOwnProperty.call(bridgePatch, 'bridgeChannel')) {
                        await cfg3.update('bridgeChannel', Number(bridgePatch.bridgeChannel) || 1, vscode.ConfigurationTarget.Global);
                    }
                    if (Object.prototype.hasOwnProperty.call(bridgePatch, 'bridgeBotToken')) {
                        await cfg3.update('bridgeBotToken', String(bridgePatch.bridgeBotToken || ''), vscode.ConfigurationTarget.Global);
                    }
                    let bridgeProxyChanged = false;
                    if (Object.prototype.hasOwnProperty.call(bridgePatch, 'bridgeUseProxy')) {
                        const before = (0, mcpServer_1.getBridgeUseProxy)();
                        const after = await (0, mcpServer_1.setBridgeUseProxy)(Boolean(bridgePatch.bridgeUseProxy));
                        bridgeProxyChanged = before !== after;
                        try {
                            (0, mcpServer_1.writeRuntimeConfig)({
                                keepaliveEnabled: cfg3.get('keepaliveEnabled', true),
                                keepaliveMinutes: Math.max(1, Math.min(120, cfg3.get('keepaliveMinutes', 45))),
                                bridgeEnabled: cfg3.get('bridgeEnabled', false),
                                bridgeChannel: Math.max(1, Math.floor(Number(cfg3.get('bridgeChannel', 1)) || 1)),
                                bridgeBotToken: cfg3.get('bridgeBotToken', ''),
                                bridgeUseProxy: after,
                                agentTeamEnabled: (0, mcpServer_1.getPluginSettings)().agentTeamEnabled === true
                            });
                        }
                        catch (e) {
                            console.warn('[QingTian] 同步桥接代理运行时开关失败', e);
                        }
                        if (bridgeProxyChanged) {
                            try {
                                require('./extension').restartBridgeProcess?.();
                            }
                            catch { }
                        }
                    }
                    webviewView.webview.postMessage({
                        command: 'bridgeSettings',
                        data: {
                            bridgeEnabled: cfg3.get('bridgeEnabled', false),
                            bridgeChannel: cfg3.get('bridgeChannel', 1),
                            bridgeBotToken: cfg3.get('bridgeBotToken', ''),
                            bridgeUseProxy: (0, mcpServer_1.getBridgeUseProxy)()
                        }
                    });
                    break;
                }
                case 'restartServer': {
                    if (!await this._ensureActivated('刷新配置')) {
                        break;
                    }
                    vscode.commands.executeCommand('qingtian.restartServer');
                    break;
                }
                case 'reloadWindow': {
                    vscode.commands.executeCommand('workbench.action.reloadWindow');
                    break;
                }
                case 'fullCleanup': {
                    // 不做激活校验：清理是重置动作，即使授权失效也应允许。
                    vscode.commands.executeCommand('qingtian.fullCleanup');
                    break;
                }
                case 'openWebInBrowser': {
                    vscode.commands.executeCommand('qingtian.openWebInBrowser');
                    break;
                }
                case 'openPurchaseLink': {
                    const wa = String(msg?.url || 'https://chat.whatsapp.com/EJUfSlyZxlQ0bYCtdYqsQA').trim();
                    if (/^https?:\/\//i.test(wa)) {
                        await vscode.env.openExternal(vscode.Uri.parse(wa));
                    }
                    break;
                }
                case 'setQingTianTheme': {
                    this._themeChanged?.(String(msg.theme || '').toLowerCase() === 'dark' ? 'dark' : 'light');
                    break;
                }
                case 'setQingTianSendMode': {
                    const normalized = String(msg.sendMode || '').toLowerCase() === 'ctrl-enter' ? 'ctrl-enter' : 'enter';
                    this._initialSendMode = normalized;
                    this._sendModeChanged?.(normalized);
                    break;
                }
                case 'setQingTianLanguage': {
                    try {
                        const ctx = require('./extension').getExtensionContext?.();
                        const lang = String(msg.language || '').toLowerCase() === 'en' ? 'en' : 'zh';
                        ctx?.globalState?.update('qingtian.uiLanguage', lang);
                        require('./agentTeamWorkbench').setAgentTeamWorkbenchLanguage?.(lang);
                    }
                    catch { }
                    break;
                }
                case 'openAgentTeamWorkbench': {
                    vscode.commands.executeCommand('qingtian.openAgentTeamWorkbench');
                    break;
                }
                case 'sendAgentTeamGroupMessage': {
                    const result = (0, mcpServer_1.sendAgentTeamGroupMessage)({
                        groupId: String(msg.groupId || ''),
                        text: String(msg.text || ''),
                        author: '鐢ㄦ埛'
                    });
                    webviewView.webview.postMessage({
                        command: 'agentTeamSendResult',
                        data: result
                    });
                    webviewView.webview.postMessage({
                        command: 'agentTeamSnapshot',
                        data: (0, mcpServer_1.getAgentTeamWorkbenchSnapshot)(String(msg.groupId || ''))
                    });
                    break;
                }
                case 'updateAgentRole': {
                    const result = (0, mcpServer_1.updateAgentRole)(String(msg.channelId || ''), String(msg.roleId || ''), String(msg.groupId || ''));
                    webviewView.webview.postMessage({
                        command: 'agentTeamRoleResult',
                        data: result
                    });
                    webviewView.webview.postMessage({
                        command: 'agentTeamSnapshot',
                        data: (0, mcpServer_1.getAgentTeamWorkbenchSnapshot)(String(msg.groupId || ''))
                    });
                    break;
                }
                case 'updateNoticeToggleSuppress': {
                    this._updateNoticeHandlers?.toggleSuppress(msg.checked === true);
                    break;
                }
                case 'updateNoticeOpenAction': {
                    await this._updateNoticeHandlers?.openAction();
                    break;
                }
                case 'updateNoticeClose': {
                    await this._updateNoticeHandlers?.close();
                    break;
                }
                case 'updateNoticeOpenExternal': {
                    const rawUrl = String(msg.url || '').trim();
                    if (/^(https?:|mailto:)/i.test(rawUrl)) {
                        await vscode.env.openExternal(vscode.Uri.parse(rawUrl));
                    }
                    break;
                }
                case 'updateWebServerSettings': {
                    const cfg4 = vscode.workspace.getConfiguration('qingtian');
                    const patch = msg?.data || {};
                    if (Object.prototype.hasOwnProperty.call(patch, 'webServerEnabled')) {
                        await cfg4.update('webServerEnabled', Boolean(patch.webServerEnabled), vscode.ConfigurationTarget.Global);
                    }
                    if (Object.prototype.hasOwnProperty.call(patch, 'webServerPort')) {
                        const p = Number(patch.webServerPort);
                        if (Number.isFinite(p) && p >= 1024 && p <= 65535) {
                            await cfg4.update('webServerPort', p, vscode.ConfigurationTarget.Global);
                        }
                    }
                    break;
                }
                case 'addChannel': {
                    const result = await vscode.commands.executeCommand('qingtian.addChannel');
                    webviewView.webview.postMessage({
                        command: 'channelActionResult',
                        action: 'add',
                        ...result
                    });
                    break;
                }
                case 'removeChannel': {
                    const result = await vscode.commands.executeCommand('qingtian.removeChannel');
                    webviewView.webview.postMessage({
                        command: 'channelActionResult',
                        action: 'remove',
                        ...result
                    });
                    break;
                }
                case 'fetchCursorUsage': {
                    try {
                        webviewView.webview.postMessage({ command: 'cursorUsageLoading' });
                        const usageResult = await (0, cursorUsage_1.fetchCursorUsage)();
                        webviewView.webview.postMessage({ command: 'cursorUsageData', data: usageResult });
                    }
                    catch (e) {
                        webviewView.webview.postMessage({ command: 'cursorUsageData', data: { profile: null, usage: null, error: e.message || '鏈煡閿欒' } });
                    }
                    break;
                }
                case 'getAccounts': {
                    const accounts = (0, cursorAccounts_1.loadAccounts)();
                    webviewView.webview.postMessage({ command: 'accountList', accounts });
                    webviewView.webview.postMessage(buildSeamlessStatusPayload());
                    // 后台异步检查未标记的 token，检测到失效后持久化。
                    (async () => {
                        let changed = false;
                        await Promise.all(accounts.map(async (a) => {
                            if (a.tokenExpired) {
                                return;
                            }
                            try {
                                const h = await (0, cursorAccounts_1.checkTokenHealth)(a.token);
                                if (!h.valid) {
                                    a.tokenExpired = true;
                                    (0, cursorAccounts_1.markTokenExpired)(a.id);
                                    changed = true;
                                }
                            }
                            catch { /* ignore */ }
                        }));
                        if (changed) {
                            webviewView.webview.postMessage({ command: 'accountList', accounts });
                        }
                    })();
                    break;
                }
                case 'refreshAccounts': {
                    // 重置所有失效标记，重新检测。
                    (0, cursorAccounts_1.resetTokenExpiredFlags)();
                    const accts = (0, cursorAccounts_1.loadAccounts)();
                    webviewView.webview.postMessage({ command: 'accountList', accounts: accts });
                    webviewView.webview.postMessage(buildSeamlessStatusPayload());
                    (async () => {
                        let changed = false;
                        await Promise.all(accts.map(async (a) => {
                            try {
                                const h = await (0, cursorAccounts_1.checkTokenHealth)(a.token);
                                if (!h.valid) {
                                    a.tokenExpired = true;
                                    (0, cursorAccounts_1.markTokenExpired)(a.id);
                                    changed = true;
                                }
                            }
                            catch { /* ignore */ }
                        }));
                        if (changed) {
                            webviewView.webview.postMessage({ command: 'accountList', accounts: accts });
                        }
                    })();
                    break;
                }
                case 'getSeamlessStatus': {
                    webviewView.webview.postMessage(buildSeamlessStatusPayload());
                    break;
                }
                case 'enableSeamlessSwitch': {
                    const res = await (0, seamlessSwitch_1.injectWorkbench)();
                    webviewView.webview.postMessage({
                        command: 'seamlessInjectResult',
                        ok: res.ok,
                        message: res.message
                    });
                    webviewView.webview.postMessage(buildSeamlessStatusPayload());
                    setTimeout(() => {
                        webviewView.webview.postMessage(buildSeamlessStatusPayload());
                    }, 500);
                    break;
                }
                case 'disableSeamlessSwitch': {
                    const res = await (0, seamlessSwitch_1.restoreWorkbench)();
                    webviewView.webview.postMessage({
                        command: 'seamlessRestoreResult',
                        ok: res.ok,
                        message: res.message
                    });
                    webviewView.webview.postMessage(buildSeamlessStatusPayload());
                    setTimeout(() => {
                        webviewView.webview.postMessage(buildSeamlessStatusPayload());
                    }, 500);
                    break;
                }
                case 'saveCurrentAccount': {
                    const res = await (0, cursorAccounts_1.saveCurrentAccount)(msg.name);
                    webviewView.webview.postMessage({ command: 'accountActionResult', action: 'save', ...res });
                    if (res.accounts) {
                        webviewView.webview.postMessage({ command: 'accountList', accounts: res.accounts });
                    }
                    break;
                }
                case 'addTokenAccount': {
                    const res = await (0, cursorAccounts_1.addTokenAccount)(msg.name || '', msg.token || '');
                    webviewView.webview.postMessage({ command: 'accountActionResult', action: 'add', ...res });
                    if (res.accounts) {
                        webviewView.webview.postMessage({ command: 'accountList', accounts: res.accounts });
                    }
                    break;
                }
                case 'addSessionTokenAccount': {
                    const res = await (0, cursorAccounts_1.addSessionTokenAccount)(msg.sessionToken || '');
                    webviewView.webview.postMessage({ command: 'accountActionResult', action: 'add-session', ...res });
                    if (res.accounts) {
                        webviewView.webview.postMessage({ command: 'accountList', accounts: res.accounts });
                    }
                    break;
                }
                case 'switchAccount': {
                    const res = await (0, cursorAccounts_1.switchAccount)(msg.id);
                    webviewView.webview.postMessage({ command: 'accountActionResult', action: 'switch', ...res });
                    if (res.ok) {
                        if (res.seamless) {
                            // 无感模式：不需要任何重启。
                            webviewView.webview.postMessage({ command: 'accountList', accounts: (await Promise.resolve().then(() => __importStar(require('./cursorAccounts')))).loadAccounts() });
                        }
                        else if (res.needReload) {
                            vscode.window.showInformationMessage(uiText('正在切换账号，Cursor 将自动重启...'));
                            setTimeout(() => {
                                vscode.commands.executeCommand('workbench.action.quit');
                            }, 1000);
                        }
                    }
                    break;
                }
                case 'restartSwitchAccount': {
                    const res = await (0, cursorAccounts_1.restartSwitchAccount)(msg.id);
                    webviewView.webview.postMessage({ command: 'accountActionResult', action: 'restart-switch', ...res });
                    if (res.ok) {
                        webviewView.webview.postMessage({ command: 'accountList', accounts: (0, cursorAccounts_1.loadAccounts)() });
                        vscode.window.showInformationMessage(uiText('正在重启切号，Cursor 将关闭后自动重新打开...'));
                        setTimeout(() => {
                            vscode.commands.executeCommand('workbench.action.quit');
                        }, 1000);
                    }
                    break;
                }
                case 'deleteAccount': {
                    const res = (0, cursorAccounts_1.deleteAccount)(msg.id);
                    webviewView.webview.postMessage({ command: 'accountActionResult', action: 'delete', ...res });
                    webviewView.webview.postMessage({ command: 'accountList', accounts: res.accounts });
                    break;
                }
                case 'renameAccount': {
                    const res = (0, cursorAccounts_1.renameAccount)(msg.id, msg.name || '');
                    webviewView.webview.postMessage({ command: 'accountList', accounts: res.accounts });
                    break;
                }
                case 'checkTokenHealth': {
                    const health = await (0, cursorAccounts_1.checkTokenHealth)(msg.token || '');
                    webviewView.webview.postMessage({ command: 'tokenHealthResult', id: msg.id, ...health });
                    break;
                }
                case 'exportAccounts': {
                    const res = (0, cursorAccounts_1.exportAccounts)();
                    if (res.ok) {
                        await vscode.env.clipboard.writeText(res.data);
                        vscode.window.showInformationMessage(uiText('账号列表已复制到剪贴板'));
                    }
                    break;
                }
                case 'importAccounts': {
                    const clip = await vscode.env.clipboard.readText();
                    const res = (0, cursorAccounts_1.importAccounts)(clip);
                    webviewView.webview.postMessage({ command: 'accountActionResult', action: 'import', ...res });
                    if (res.accounts) {
                        webviewView.webview.postMessage({ command: 'accountList', accounts: res.accounts });
                    }
                    break;
                }
                case 'refreshBilling': {
                    // 刷新单个账号的 billing 数据。
                    const accts = (0, cursorAccounts_1.loadAccounts)();
                    const target = accts.find((a) => a.id === msg.id);
                    if (target) {
                        webviewView.webview.postMessage({ command: 'billingRefreshing', id: msg.id });
                        const billing = (0, cursorUsage_1.accountSupportsBilling)(target.sessionToken)
                            ? await (0, cursorUsage_1.fetchAccountBilling)(target.token, target.userId, target.sessionToken)
                            : (0, cursorUsage_1.buildUnsupportedBilling)();
                        const updated = (0, cursorAccounts_1.updateAccountBilling)(target.id, billing);
                        webviewView.webview.postMessage({ command: 'accountList', accounts: updated });
                        webviewView.webview.postMessage({ command: 'billingRefreshed', id: msg.id, billing });
                    }
                    break;
                }
                case 'refreshBillingBatch': {
                    // 前端未传 ids 时刷新全部账号，避免工具栏刷新按钮无动作。
                    const allAccts = (0, cursorAccounts_1.loadAccounts)();
                    const rawIds = Array.isArray(msg.ids) ? msg.ids.map((id) => String(id)).filter(Boolean) : [];
                    const ids = rawIds.length > 0 ? rawIds : allAccts.map((a) => a.id);
                    for (const id of ids) {
                        const a = allAccts.find((x) => x.id === id);
                        if (!a) {
                            continue;
                        }
                        webviewView.webview.postMessage({ command: 'billingRefreshing', id });
                        try {
                            const billing = (0, cursorUsage_1.accountSupportsBilling)(a.sessionToken)
                                ? await (0, cursorUsage_1.fetchAccountBilling)(a.token, a.userId, a.sessionToken)
                                : (0, cursorUsage_1.buildUnsupportedBilling)();
                            (0, cursorAccounts_1.updateAccountBilling)(a.id, billing);
                            webviewView.webview.postMessage({ command: 'billingRefreshed', id, billing });
                        }
                        catch (e) {
                            webviewView.webview.postMessage({
                                command: 'billingRefreshed',
                                id,
                                billing: { error: e?.message || '刷新失败' }
                            });
                        }
                    }
                    webviewView.webview.postMessage({ command: 'accountList', accounts: (0, cursorAccounts_1.loadAccounts)() });
                    break;
                }
                case 'deleteBillingBatch': {
                    const ids = msg.ids || [];
                    if (ids.length > 0) {
                        const res = (0, cursorAccounts_1.deleteAccounts)(ids);
                        webviewView.webview.postMessage({ command: 'accountList', accounts: res.accounts });
                        webviewView.webview.postMessage({ command: 'accountActionResult', action: 'delete', ...res });
                    }
                    break;
                }
                case 'exportBillingBatch': {
                    const ids = msg.ids || [];
                    const allAccts = (0, cursorAccounts_1.loadAccounts)();
                    const selected = ids.length > 0 ? allAccts.filter((a) => ids.includes(a.id)) : allAccts;
                    const exportData = selected.map((a) => ({
                        id: a.id, email: a.email, name: a.name,
                        token: a.token, refreshToken: a.refreshToken,
                        userId: a.userId, membershipType: a.membershipType,
                        signUpType: a.signUpType, savedAt: a.savedAt,
                        sessionToken: a.sessionToken || undefined,
                        sessionExpiredAt: a.sessionExpiredAt || undefined,
                        machineIds: a.machineIds || undefined,
                        billing: a.billing || null,
                    }));
                    await vscode.env.clipboard.writeText(JSON.stringify(exportData, null, 2));
                    vscode.window.showInformationMessage(uiText(`已导出 ${exportData.length} 个账号到剪贴板`));
                    break;
                }
                case 'openBillingDetail': {
                    const accts2 = (0, cursorAccounts_1.loadAccounts)();
                    const tgt = accts2.find((a) => a.id === msg.id);
                    if (tgt) {
                        webviewView.webview.postMessage({ command: 'billingDetail', id: msg.id, account: tgt });
                        if (!(0, cursorUsage_1.accountSupportsBilling)(tgt.sessionToken)) {
                            break;
                        }
                        // 如果没有缓存或缓存过期（>5分钟），自动刷新。
                        if (!tgt.billing || (Date.now() - (tgt.billing.fetchedAt || 0)) > 300000) {
                            webviewView.webview.postMessage({ command: 'billingRefreshing', id: msg.id });
                            const billing = await (0, cursorUsage_1.fetchAccountBilling)(tgt.token, tgt.userId, tgt.sessionToken);
                            const updated = (0, cursorAccounts_1.updateAccountBilling)(tgt.id, billing);
                            const freshAcct = updated.find((a) => a.id === msg.id);
                            webviewView.webview.postMessage({ command: 'billingDetail', id: msg.id, account: freshAcct });
                            webviewView.webview.postMessage({ command: 'accountList', accounts: updated });
                        }
                    }
                    break;
                }
                case 'copyCurrentToken': {
                    const res = await (0, cursorAccounts_1.getCurrentToken)();
                    if (res.ok && res.token) {
                        await vscode.env.clipboard.writeText(res.token);
                        vscode.window.showInformationMessage(uiText('当前 Token 已复制到剪贴板'));
                    }
                    else {
                        vscode.window.showErrorMessage(uiText(res.message || '获取 Token 失败'));
                    }
                    break;
                }
                case 'clearHistory': {
                    const clearChId = msg.channelId;
                    if (clearChId === 'all') {
                        const count = (0, mcpServer_1.getChannelCount)();
                        for (let i = 1; i <= count; i++)
                            (0, mcpServer_1.clearQueue)(String(i));
                    }
                    else if (clearChId) {
                        (0, mcpServer_1.clearQueue)(clearChId);
                    }
                    webviewView.webview.postMessage({ command: 'historyCleared' });
                    break;
                }
                case 'copyHistoryItem': {
                    try {
                        await vscode.env.clipboard.writeText(String(msg.text || ''));
                        webviewView.webview.postMessage({ command: 'historyItemCopied', ok: true, id: msg.id });
                    }
                    catch (e) {
                        webviewView.webview.postMessage({
                            command: 'historyItemCopied',
                            ok: false,
                            id: msg.id,
                            message: e instanceof Error ? e.message : String(e)
                        });
                    }
                    break;
                }
                case 'activateLicense': {
                    const code = msg.code;
                    if (code) {
                        const { activate: activateLicense, getLicenseCountdownStatus, getPaymentInfo } = require('./activation');
                        const result = await activateLicense(code);
                        webviewView.webview.postMessage({
                            command: 'licenseResult',
                            success: result.success,
                            message: result.message
                        });
                        if (result.success) {
                            const status = (0, statusPayload_1.buildStatusPayload)(this._getDisplayVersion());
                            webviewView.webview.postMessage({
                                command: 'licenseStatus',
                                activated: true,
                                payment: this._paymentPayload(webviewView),
                                license: getLicenseCountdownStatus()
                            });
                            webviewView.webview.postMessage({
                                command: 'status',
                                data: {
                                    ...status,
                                    license: getLicenseCountdownStatus()
                                }
                            });
                        }
                        else {
                            webviewView.webview.postMessage({
                                command: 'licenseStatus',
                                activated: false,
                                payment: this._paymentPayload(webviewView),
                                license: getLicenseCountdownStatus()
                            });
                        }
                    }
                    break;
                }
                case 'startLocalTrial': {
                    const { startLocalTrial, getLicenseCountdownStatus, getPaymentInfo } = require('./activation');
                    const result = startLocalTrial();
                    webviewView.webview.postMessage({
                        command: 'licenseResult',
                        success: result.success,
                        message: result.message
                    });
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: result.success === true,
                        payment: this._paymentPayload(webviewView),
                        license: getLicenseCountdownStatus()
                    });
                    if (result.success) {
                        const status = (0, statusPayload_1.buildStatusPayload)(this._getDisplayVersion());
                        webviewView.webview.postMessage({
                            command: 'status',
                            data: { ...status, license: getLicenseCountdownStatus() }
                        });
                    }
                    break;
                }
                case 'selectPaymentPlan': {
                    const { setSelectedPaymentPlan, getLicenseCountdownStatus, isActivated, hasConsumedTrial } = require('./activation');
                    const result = setSelectedPaymentPlan(msg.planId);
                    webviewView.webview.postMessage({
                        command: 'licenseResult',
                        success: result.success,
                        message: result.success
                            ? `Selected ${result.plan.label} — $${result.plan.amount} / ${result.plan.days} days`
                            : (result.message || 'Failed to select plan')
                    });
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: isActivated(),
                        preview: msg.preview === true || !isActivated(),
                        payment: this._paymentPayload(webviewView),
                        license: getLicenseCountdownStatus(),
                        trialConsumed: hasConsumedTrial()
                    });
                    break;
                }
                case 'forceExpireForTest': {
                    const { forceExpireLocalLicense, getLicenseCountdownStatus, hasConsumedTrial } = require('./activation');
                    const result = forceExpireLocalLicense();
                    webviewView.webview.postMessage({
                        command: 'licenseResult',
                        success: true,
                        message: result.message || 'Expired for testing'
                    });
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: false,
                        payment: this._paymentPayload(webviewView),
                        license: getLicenseCountdownStatus(),
                        trialConsumed: hasConsumedTrial()
                    });
                    this.focusPanel();
                    break;
                }
                case 'debugUnlockSelectedPlan': {
                    const { debugUnlockSelectedPlan, getLicenseCountdownStatus, isActivated } = require('./activation');
                    const result = debugUnlockSelectedPlan('ui-test');
                    webviewView.webview.postMessage({
                        command: 'licenseResult',
                        success: result.success,
                        message: result.message
                    });
                    if (result.success) this._renewalPreview = false;
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: isActivated(),
                        exitPreview: result.success === true,
                        payment: this._paymentPayload(webviewView),
                        license: getLicenseCountdownStatus()
                    });
                    if (result.success) {
                        const status = (0, statusPayload_1.buildStatusPayload)(this._getDisplayVersion());
                        webviewView.webview.postMessage({
                            command: 'status',
                            data: { ...status, license: getLicenseCountdownStatus() }
                        });
                    }
                    break;
                }
                case 'showRenewalPanel': {
                    const { getPaymentInfo, getLicenseCountdownStatus, isActivated, hasConsumedTrial } = require('./activation');
                    const payment = this._paymentPayload(webviewView);
                    this._renewalPreview = true;
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: false,
                        preview: true,
                        message: 'Renew with USDT-TRC20. After payment, click Verify payment.',
                        payment,
                        license: getLicenseCountdownStatus(),
                        trialConsumed: hasConsumedTrial(),
                        wasActivated: isActivated()
                    });
                    webviewView.webview.postMessage({
                        command: 'licenseResult',
                        success: true,
                        pending: true,
                        message: `Payment address (USDT-TRC20): ${payment.address || '(missing)'} — Monthly $9.9 / Yearly $49`
                    });
                    try {
                        vscode.window.showInformationMessage(
                            `SlashSubs pay USDT-TRC20 to ${payment.address} (Monthly $9.9 / Yearly $49)`
                        );
                    } catch { }
                    this.focusPanel();
                    break;
                }
                case 'exitRenewalPreview': {
                    this._renewalPreview = false;
                    break;
                }
                case 'getCryptoMethods': {
                    const { getCryptoAddresses } = require('./activation');
                    const methods = getCryptoAddresses();
                    webviewView.webview.postMessage({
                        command: 'cryptoMethods',
                        methods: methods
                    });
                    break;
                }
                case 'selectCryptoMethod': {
                    const { getPaymentInfoForMethod } = require('./activation');
                    const methodId = String(message.methodId || '');
                    const info = getPaymentInfoForMethod(methodId);
                    webviewView.webview.postMessage({
                        command: 'cryptoMethodDetails',
                        ...info
                    });
                    break;
                }
                case 'copyCryptoAddress': {
                    const { getPaymentInfoForMethod } = require('./activation');
                    const info = getPaymentInfoForMethod(String(message.methodId || ''));
                    if (info.configured && info.address) {
                        await vscode.env.clipboard.writeText(info.address);
                        webviewView.webview.postMessage({
                            command: 'licenseResult',
                            success: true,
                            message: info.methodLabel + ' address copied!'
                        });
                    } else {
                        webviewView.webview.postMessage({
                            command: 'licenseResult',
                            success: false,
                            message: 'Address not configured for ' + (info.methodLabel || message.methodId)
                        });
                    }
                    break;
                }
                case 'getPaymentInfo': {
                    const { getPaymentInfo, getLicenseCountdownStatus, isActivated } = require('./activation');
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: isActivated(),
                        payment: this._paymentPayload(webviewView),
                        license: getLicenseCountdownStatus()
                    });
                    break;
                }
                case 'copyPaymentAddress': {
                    const { getPaymentInfo } = require('./activation');
                    const payment = getPaymentInfo();
                    if (payment.address) {
                        await vscode.env.clipboard.writeText(payment.address);
                        webviewView.webview.postMessage({
                            command: 'licenseResult',
                            success: true,
                            message: '收款地址已复制'
                        });
                    }
                    else {
                        webviewView.webview.postMessage({
                            command: 'licenseResult',
                            success: false,
                            message: '未配置收款地址（设置 qingtian.paymentAddress）'
                        });
                    }
                    break;
                }
                case 'verifyCryptoPayment': {
                    const { verifyAndRenewCryptoPayment, getLicenseCountdownStatus, isActivated } = require('./activation');
                    const result = await verifyAndRenewCryptoPayment();
                    webviewView.webview.postMessage({
                        command: 'licenseResult',
                        success: result.success,
                        message: result.message,
                        pending: result.pending === true
                    });
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: isActivated(),
                        payment: this._paymentPayload(webviewView),
                        license: getLicenseCountdownStatus()
                    });
                    if (result.success) {
                        this._renewalPreview = false;
                        webviewView.webview.postMessage({
                            command: 'licenseStatus',
                            activated: true,
                            exitPreview: true,
                            payment: this._paymentPayload(webviewView),
                            license: getLicenseCountdownStatus()
                        });
                        const status = (0, statusPayload_1.buildStatusPayload)(this._getDisplayVersion());
                        webviewView.webview.postMessage({
                            command: 'status',
                            data: { ...status, license: getLicenseCountdownStatus() }
                        });
                    }
                    break;
                }
                case 'logout': {
                    const confirmSel = await vscode.window.showWarningMessage(uiText('确定要退出授权吗？退出后若试用/付费已到期需重新续费。'), { modal: true }, uiText('确认退出'));
                    if (confirmSel !== uiText('确认退出')) {
                        break;
                    }
                    const { clearLicense } = require('./activation');
                    clearLicense();
                    webviewView.webview.postMessage({
                        command: 'licenseStatus',
                        activated: false,
                        message: '已退出授权'
                    });
                    vscode.window.showInformationMessage(uiText('已退出激活，请重新加载窗口。'), uiText('重新加载')).then(sel => {
                        if (sel === uiText('重新加载')) {
                            vscode.commands.executeCommand('workbench.action.reloadWindow');
                        }
                    });
                    break;
                }
                case 'selectFiles': {
                    const uris = await vscode.window.showOpenDialog({
                        canSelectMany: true,
                        canSelectFiles: true,
                        canSelectFolders: false,
                        openLabel: uiText('选择文件')
                    });
                    if (uris && uris.length > 0) {
                        const files = uris.map(u => ({
                            path: u.fsPath,
                            name: u.fsPath,
                            displayName: path.basename(u.fsPath),
                            type: this._getFileType(u.fsPath)
                        }));
                        webviewView.webview.postMessage({ command: 'filesSelected', data: files });
                    }
                    break;
                }
                case 'selectFolder': {
                    const folderUris = await vscode.window.showOpenDialog({
                        canSelectMany: false,
                        canSelectFiles: false,
                        canSelectFolders: true,
                        openLabel: uiText('选择文件夹')
                    });
                    if (folderUris && folderUris.length > 0) {
                        const folderPath = folderUris[0].fsPath;
                        try {
                            const entries = fs.readdirSync(folderPath);
                            const files = entries
                                .filter((e) => {
                                const full = path.join(folderPath, e);
                                return fs.statSync(full).isFile();
                            })
                                .map((e) => {
                                const full = path.join(folderPath, e);
                                return {
                                    path: full,
                                    name: full,
                                    displayName: e,
                                    type: this._getFileType(full)
                                };
                            });
                            if (files.length > 0) {
                                webviewView.webview.postMessage({ command: 'filesSelected', data: files });
                            }
                        }
                        catch (e) {
                            console.error('[QingTian] 读取文件夹失败', e);
                        }
                    }
                    break;
                }
                case 'pasteImage': {
                    try {
                        const { base64, type } = msg.data;
                        if (!base64)
                            break;
                        this._savePastedImage(webviewView.webview, String(base64), String(type || ''));
                    }
                    catch (e) {
                        console.error('[QingTian] 绮樿创鍥剧墖澶辫触:', e);
                        vscode.window.showErrorMessage(uiText('粘贴图片失败: ' + e.message));
                        webviewView.webview.postMessage({
                            command: 'pasteImageFailed',
                            message: '粘贴图片失败: ' + e.message
                        });
                    }
                    break;
                }
                case 'pasteImageChunk': {
                    try {
                        this._cleanupStalePasteImages();
                        const data = msg.data || {};
                        const id = String(data.id || '').trim();
                        const type = String(data.type || 'image/png');
                        const index = Number(data.index);
                        const total = Number(data.total);
                        const chunk = typeof data.chunk === 'string' ? data.chunk : '';
                        if (!id || !Number.isInteger(index) || !Number.isInteger(total) || total <= 0 || index < 0 || index >= total || !chunk) {
                            break;
                        }
                        let pending = this._pendingPasteImages.get(id);
                        if (!pending || pending.total !== total) {
                            pending = {
                                chunks: new Array(total).fill(''),
                                total,
                                type,
                                received: 0,
                                createdAt: Date.now()
                            };
                            this._pendingPasteImages.set(id, pending);
                        }
                        if (!pending.chunks[index]) {
                            pending.chunks[index] = chunk;
                            pending.received += 1;
                        }
                        if (pending.received >= pending.total) {
                            this._pendingPasteImages.delete(id);
                            this._savePastedImage(webviewView.webview, pending.chunks.join(''), pending.type);
                        }
                    }
                    catch (e) {
                        console.error('[QingTian] 绮樿创鍥剧墖鍒嗙墖澶辫触:', e);
                        vscode.window.showErrorMessage(uiText('粘贴图片失败: ' + e.message));
                        webviewView.webview.postMessage({
                            command: 'pasteImageFailed',
                            message: '粘贴图片失败: ' + e.message
                        });
                    }
                    break;
                }
                case 'searchFiles': {
                    const keyword = String(msg.data?.keyword || '').trim().toLowerCase();
                    try {
                        const excludePattern = '{**/node_modules/**,**/.git/**,**/out/**,**/dist/**,**/*.vsix}';
                        const uris = await vscode.workspace.findFiles('**/*', excludePattern, 200);
                        const files = uris
                            .map(u => ({
                            path: vscode.workspace.asRelativePath(u),
                            fullPath: u.fsPath,
                            name: path.basename(u.fsPath)
                        }))
                            .filter(file => {
                            if (!keyword)
                                return true;
                            return file.name.toLowerCase().includes(keyword) || file.path.toLowerCase().includes(keyword);
                        })
                            .sort((a, b) => {
                            const aStarts = keyword ? a.name.toLowerCase().startsWith(keyword) : false;
                            const bStarts = keyword ? b.name.toLowerCase().startsWith(keyword) : false;
                            if (aStarts !== bStarts)
                                return aStarts ? -1 : 1;
                            return a.path.length - b.path.length;
                        })
                            .slice(0, 20);
                        webviewView.webview.postMessage({ command: 'fileSearchResults', data: files });
                    }
                    catch {
                        webviewView.webview.postMessage({ command: 'fileSearchResults', data: [] });
                    }
                    break;
                }
                case 'exportHistory': {
                    try {
                        const history = msg.data || [];
                        if (history.length === 0) {
                            vscode.window.showWarningMessage(uiText('暂无对话记录可导出'));
                            break;
                        }
                        const now = new Date();
                        const pad = (n) => String(n).padStart(2, '0');
                        const timeStr = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;
                        let md = `# QingTian 瀵硅瘽璁板綍\n瀵煎嚭鏃堕棿: ${timeStr}\n\n---\n\n`;
                        for (const item of history) {
                            const t = new Date(item.timestamp);
                            const ts = `${pad(t.getHours())}:${pad(t.getMinutes())}:${pad(t.getSeconds())}`;
                            md += `## [${ts}] ${item.title || '未命名'}\n\n`;
                            if (item.summary)
                                md += `${item.summary}\n\n`;
                            if (item.userResponse)
                                md += `**鐢ㄦ埛鍥炲**: ${item.userResponse}\n\n`;
                            md += `---\n\n`;
                        }
                        const uri = await vscode.window.showSaveDialog({
                            defaultUri: vscode.Uri.file(path.join(os.homedir(), `qingtian-${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}.md`)),
                            filters: { 'Markdown': ['md'] }
                        });
                        if (uri) {
                            fs.writeFileSync(uri.fsPath, md, 'utf8');
                            vscode.window.showInformationMessage(uiText(`Conversation history exported to ${uri.fsPath}`));
                        }
                    }
                    catch (e) {
                        console.error('[QingTian] 瀵煎嚭鍘嗗彶澶辫触:', e);
                        vscode.window.showErrorMessage(uiText('导出失败: ' + e.message));
                    }
                    break;
                }
                case 'getQuickCommands': {
                    webviewView.webview.postMessage({
                        command: 'quickCommands',
                        data: (0, quickCommands_1.getQuickCommands)()
                    });
                    break;
                }
                case 'addQuickCommand': {
                    const label = String(msg?.data?.label || '').trim();
                    const text = String(msg?.data?.text || '');
                    const res = await (0, quickCommands_1.addQuickCommand)(label, text);
                    webviewView.webview.postMessage({
                        command: 'quickCommandResult',
                        action: 'add',
                        ok: res.ok,
                        message: res.message,
                        data: res.list
                    });
                    break;
                }
                case 'removeQuickCommand': {
                    const id = String(msg?.data?.id || '').trim();
                    const res = await (0, quickCommands_1.removeQuickCommand)(id);
                    webviewView.webview.postMessage({
                        command: 'quickCommandResult',
                        action: 'remove',
                        ok: res.ok,
                        message: res.message,
                        data: res.list
                    });
                    break;
                }
                case 'updateQuickCommand': {
                    const id = String(msg?.data?.id || '').trim();
                    const patch = {
                        label: typeof msg?.data?.label === 'string' ? msg.data.label : undefined,
                        text: typeof msg?.data?.text === 'string' ? msg.data.text : undefined
                    };
                    const res = await (0, quickCommands_1.updateQuickCommand)(id, patch);
                    webviewView.webview.postMessage({
                        command: 'quickCommandResult',
                        action: 'update',
                        ok: res.ok,
                        message: res.message,
                        data: res.list
                    });
                    break;
                }
                case 'resetQuickCommands': {
                    const list = await (0, quickCommands_1.resetQuickCommands)();
                    webviewView.webview.postMessage({
                        command: 'quickCommandResult',
                        action: 'reset',
                        ok: true,
                        data: list
                    });
                    break;
                }
                case 'batchRetryLoadOpts': {
                    try {
                        const ctx = require('./extension').getExtensionContext?.();
                        const saved = normalizeBatchRetryOpts(ctx?.globalState?.get('qingtian.batchRetryOpts') || {});
                        webviewView.webview.postMessage({ command: 'batchRetryFillOpts', data: saved });
                    }
                    catch {
                        webviewView.webview.postMessage({ command: 'batchRetryFillOpts', data: {} });
                    }
                    break;
                }
                case 'batchRetryLoadHistory': {
                    webviewView.webview.postMessage({
                        command: 'batchRetryHistory',
                        data: this._getBatchRetryHistory()
                    });
                    break;
                }
                case 'batchRetryLoadNetworkState': {
                    this.postBatchRetryNetworkState();
                    break;
                }
                case 'batchRetrySetNetworkType': {
                    const result = await this._setBatchRetryNetworkType(String(msg.value || ''));
                    webviewView.webview.postMessage({
                        command: 'batchRetryNetworkSetResult',
                        data: result
                    });
                    this.postBatchRetryNetworkState();
                    break;
                }
                case 'batchRetryStart': {
                    if (!await this._ensureActivated('批量会话重试'))
                        break;
                    const platformBlocker = this._getBatchRetryPlatformBlocker();
                    if (platformBlocker) {
                        webviewView.webview.postMessage({
                            command: 'batchRetryStatus',
                            data: {
                                text: platformBlocker,
                                done: true,
                                tasks: [],
                                sessionComposerBindings: [],
                                count: '0/0',
                                error: true
                            }
                        });
                        break;
                    }
                    await (0, seamlessSwitch_1.refreshPrimaryRuntimeProbe)().catch(() => { });
                    const injectionStatus = (0, seamlessSwitch_1.getSeamlessInjectionStatus)();
                    const runtimeStatus = (0, seamlessSwitch_1.getSeamlessRuntimeStatus)();
                    if (!injectionStatus.current || !runtimeStatus.runtimeLoaded) {
                        webviewView.webview.postMessage({
                            command: 'batchRetryStatus',
                            data: {
                                text: this._getBatchRetryPreflightMessage(),
                                done: true,
                                tasks: [],
                                sessionComposerBindings: [],
                                count: '0/0',
                                error: true,
                                requiresInjection: true
                            }
                        });
                        webviewView.webview.postMessage(buildSeamlessStatusPayload());
                        break;
                    }
                    const engine = this._batchRetryEngine;
                    if (!engine) {
                        webviewView.webview.postMessage({ command: 'batchRetryStatus', data: { text: '引擎未初始化', done: true, tasks: [], sessionComposerBindings: [], count: '0/0' } });
                        break;
                    }
                    const focusedAdoption = await this._adoptFocusedBatchWindow('batch_start');
                    const focusedTargetClientId = focusedAdoption.ok && focusedAdoption.clientId
                        ? String(focusedAdoption.clientId)
                        : '';
                    const focusedTargetClientSeenAfter = focusedTargetClientId
                        ? (Number(focusedAdoption.adoptedAt || 0) || 0)
                        : 0;
                    this._clearBatchRetryPendingRun();
                    const opts = normalizeBatchRetryOpts(msg.opts || {});
                    this._batchRetryHistoryRecordedTaskIds.clear();
                    this._batchRetryNotifiedTaskIds.clear();
                    // 保存选项
                    try {
                        const ctx = require('./extension').getExtensionContext?.();
                        ctx?.globalState?.update('qingtian.batchRetryOpts', opts);
                    }
                    catch { }
                    const rawSessions = Array.isArray(msg.sessions) ? msg.sessions : [];
                    const requestedSessionCount = Math.max(1, Math.min(50, opts.sessionCount || rawSessions.length || (0, mcpServer_1.getChannelCount)() || 1));
                    const blueLightChannelIds = new Set((Array.isArray(msg.blueLightChannelIds) ? msg.blueLightChannelIds : [])
                        .map((id) => String(id || '').trim())
                        .filter(Boolean));
                    const runId = 'br-' + Date.now().toString(36);
                    this._logBatchRetry('webview batch retry start requested', {
                        runId,
                        requestedSessionCount,
                        opts,
                        rawSessionCount: rawSessions.length,
                        blueLightChannelIds: Array.from(blueLightChannelIds),
                        focusedTargetClientId,
                        focusedTargetClientSeenAfter,
                        focusedAdoption,
                        configuredChannelCount: (0, mcpServer_1.getChannelCount)(),
                        queueRoot: (0, mcpServer_1.getQueueRoot)()
                    });
                    const rawByChannel = new Map();
                    rawSessions.forEach((raw, index) => {
                        const channelId = String(raw?.channelId || (index + 1)).trim();
                        if (channelId && !rawByChannel.has(channelId))
                            rawByChannel.set(channelId, raw);
                    });
                    const compareChannelIds = (a, b) => {
                        const na = Number(a);
                        const nb = Number(b);
                        if (Number.isFinite(na) && Number.isFinite(nb) && na !== nb)
                            return na - nb;
                        return String(a).localeCompare(String(b));
                    };
                    const shouldReuseBindings = opts.reuseExisting === true;
                    const isUsableBinding = (binding) => !!(binding &&
                        String(binding.channelId || '').trim() &&
                        String(binding.composerId || '').trim() &&
                        binding.exists !== false &&
                        binding.mcpWaitingActive === true);
                    // “正在等消息”判定：通道已建连并处于 MCP 等待/生成中，重试时应跳过它（别打断正在工作的通道）
                    const isBindingWaitingActive = (binding) => !!(binding && (binding.mcpWaitingActive === true || binding.connectedActive === true));
                    const bindingByChannel = new Map();
                    // 完整绑定记录（来自 localStorage 持久化，含掉线/未在等消息的通道），用于优先复用绑定的 composer
                    const bindingRecordByChannel = new Map();
                    const runtimeOccupiedByChannel = new Map();
                    for (const channelId of blueLightChannelIds) {
                        runtimeOccupiedByChannel.set(channelId, {
                            channelId,
                            composerId: '',
                            clientId: '',
                            exists: true,
                            observable: true,
                            clientMatches: true,
                            runningCheckActive: true,
                            mcpWaitingActive: true,
                            connectedActive: true,
                            runtimeOccupancyOnly: true,
                            connectionReason: 'webview_blue_light'
                        });
                    }
                    const releasedChannelIds = new Set();
                    try {
                        const bindingResult = await engine.listChannelBindings(6000, focusedTargetClientId ? {
                            targetClientId: focusedTargetClientId,
                            targetClientSeenAfter: focusedTargetClientSeenAfter
                        } : undefined);
                        this._logBatchRetry('initial channel binding scan', {
                            runId,
                            ok: bindingResult.ok,
                            bindings: (bindingResult.bindings || []).map((binding) => ({
                                channelId: String(binding?.channelId || ''),
                                composerId: String(binding?.composerId || ''),
                                clientId: String(binding?.clientId || ''),
                                clientMatches: binding?.clientMatches === true,
                                exists: binding?.exists === true,
                                observable: binding?.observable === true,
                                activeComposerMatches: binding?.activeComposerMatches === true,
                                runningCheckActive: binding?.runningCheckActive === true,
                                mcpWaitingActive: binding?.mcpWaitingActive === true,
                                mcpWaitingUpdatedAt: Number(binding?.mcpWaitingUpdatedAt || 0) || null,
                                visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                                connectedActive: binding?.connectedActive === true,
                                connectionReason: String(binding?.connectionReason || ''),
                                checkMessagesStatus: String(binding?.checkMessagesStatus || ''),
                                bottomStopButton: binding?.bottomStopButton === true,
                                bottomMicButton: binding?.bottomMicButton === true,
                                usable: isUsableBinding(binding)
                            })),
                            allBindings: (bindingResult.allBindings || []).map((binding) => ({
                                channelId: String(binding?.channelId || ''),
                                composerId: String(binding?.composerId || ''),
                                clientId: String(binding?.clientId || ''),
                                clientMatches: binding?.clientMatches === true,
                                exists: binding?.exists === true,
                                observable: binding?.observable === true,
                                activeComposerMatches: binding?.activeComposerMatches === true,
                                runningCheckActive: binding?.runningCheckActive === true,
                                mcpWaitingActive: binding?.mcpWaitingActive === true,
                                mcpWaitingUpdatedAt: Number(binding?.mcpWaitingUpdatedAt || 0) || null,
                                visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                                connectedActive: binding?.connectedActive === true,
                                connectionReason: String(binding?.connectionReason || ''),
                                checkMessagesStatus: String(binding?.checkMessagesStatus || ''),
                                bottomStopButton: binding?.bottomStopButton === true,
                                bottomMicButton: binding?.bottomMicButton === true,
                                stoppedEvidence: hasStoppedCheckMessagesEvidence(binding)
                            })),
                            occupiedChannels: (bindingResult.occupiedChannels || []).map((binding) => ({
                                channelId: String(binding?.channelId || ''),
                                composerId: String(binding?.composerId || ''),
                                clientId: String(binding?.clientId || ''),
                                clientMatches: binding?.clientMatches === true,
                                exists: binding?.exists === true,
                                observable: binding?.observable === true,
                                activeComposerMatches: binding?.activeComposerMatches === true,
                                runningCheckActive: binding?.runningCheckActive === true,
                                mcpWaitingActive: binding?.mcpWaitingActive === true,
                                mcpWaitingUpdatedAt: Number(binding?.mcpWaitingUpdatedAt || 0) || null,
                                visibleGeneratingActive: binding?.visibleGeneratingActive === true,
                                connectedActive: binding?.connectedActive === true,
                                runtimeOccupancyOnly: binding?.runtimeOccupancyOnly === true,
                                connectionReason: String(binding?.connectionReason || ''),
                                checkMessagesStatus: String(binding?.checkMessagesStatus || ''),
                                bottomStopButton: binding?.bottomStopButton === true,
                                bottomMicButton: binding?.bottomMicButton === true
                            }))
                        });
                        if (bindingResult.ok) {
                            for (const binding of bindingResult.occupiedChannels || []) {
                                const channelId = String(binding.channelId || '').trim();
                                if (!channelId || !isRuntimeOccupiedChannel(binding))
                                    continue;
                                if (!runtimeOccupiedByChannel.has(channelId)) {
                                    runtimeOccupiedByChannel.set(channelId, binding);
                                }
                            }
                            for (const binding of bindingResult.allBindings || []) {
                                const channelId = String(binding.channelId || '').trim();
                                if (!channelId)
                                    continue;
                                if (hasStoppedCheckMessagesEvidence(binding)) {
                                    releasedChannelIds.add(channelId);
                                }
                                if (hasReusableChannelBindingRecord(binding, channelId) && !bindingRecordByChannel.has(channelId)) {
                                    bindingRecordByChannel.set(channelId, binding);
                                }
                            }
                            for (const binding of bindingResult.bindings || []) {
                                if (!isUsableBinding(binding))
                                    continue;
                                const channelId = String(binding.channelId || '').trim();
                                if (channelId && !bindingByChannel.has(channelId)) {
                                    bindingByChannel.set(channelId, binding);
                                }
                                if (channelId && !bindingRecordByChannel.has(channelId)) {
                                    bindingRecordByChannel.set(channelId, binding);
                                }
                            }
                        }
                    }
                    catch (e) {
                        this._logBatchRetry('initial channel binding scan failed', {
                            runId,
                            error: e instanceof Error ? e.message : String(e)
                        });
                    }
                    const isReadyBoundBinding = (binding) => {
                        const channelId = String(binding?.channelId || '').trim();
                        if (!hasReusableChannelBindingRecord(binding, channelId))
                            return false;
                        if (isReusableChannelBindingBusy(binding))
                            return false;
                        const probe = this._probeBatchRetryChannel(channelId, releasedChannelIds.has(channelId));
                        return probe.heartbeatOnline === true;
                    };
                    const occupiedTargetByChannel = new Map();
                    const getOccupiedTarget = (channelId) => {
                        const ch = String(channelId || '').trim();
                        if (!ch)
                            return null;
                        const cached = occupiedTargetByChannel.get(ch);
                        if (cached)
                            return cached;
                        const binding = bindingRecordByChannel.get(ch);
                        const runtimeOccupied = runtimeOccupiedByChannel.get(ch);
                        const probe = this._probeBatchRetryChannel(ch, releasedChannelIds.has(ch));
                        let reason = '';
                        if (isBindingWaitingActive(binding)) {
                            reason = 'binding-waiting';
                        }
                        else if (runtimeOccupied) {
                            reason = 'runtime-occupied';
                        }
                        else if (probe.effectiveWaitingActive) {
                            reason = 'runtime-waiting-active';
                        }
                        if (!reason)
                            return null;
                        const result = { reason, probe, binding: binding || runtimeOccupied };
                        occupiedTargetByChannel.set(ch, result);
                        return result;
                    };
                    const sessionByChannel = new Map();
                    const makeSession = (channelId, binding) => {
                        const ch = String(channelId || '').trim();
                        const raw = rawByChannel.get(ch) || {};
                        const existing = sessionByChannel.get(ch);
                        const base = existing || {
                            sessionId: String(raw.sessionId || `${runId}-ch-${ch}`),
                            name: String(raw.name || `CH-${ch}`),
                            role: String(raw.role || ''),
                            agentStatus: String(raw.agentStatus || 'idle'),
                            composerId: '',
                            clientId: '',
                            online: !!raw.online,
                            channelId: ch,
                            joinPhrase: String(raw.joinPhrase || '').trim()
                                ? String(raw.joinPhrase || '')
                                : (opts.customJoinPhraseEnabled && String(opts.joinPhraseTemplate || '').trim()
                                    // 自定义重试发送语：{X}/{x} 替换为通道号（仅批量重试，不影响一键开场）
                                    ? String(opts.joinPhraseTemplate).replace(/\{x\}/gi, ch)
                                    : (0, mcpServer_1.prepareStartPrompt)(ch).prompt)
                        };
                        if (binding) {
                            base.composerId = String(binding.composerId || base.composerId || '');
                            base.clientId = String(binding.clientId || base.clientId || '');
                            base.online = true;
                            if (binding.mcpWaitingActive === true) {
                                base.initialSubmitDone = true;
                                base.alreadyMcpWaiting = true;
                            }
                        }
                        sessionByChannel.set(ch, base);
                        return base;
                    };
                    const configuredCount = Math.max(1, (0, mcpServer_1.getChannelCount)());
                    const configuredChannelIds = Array.from({ length: configuredCount }, (_, i) => String(i + 1));
                    const boundChannelIds = new Set(Array.from(bindingByChannel.keys()).map(id => String(id || '').trim()).filter(Boolean));
                    // ── 通道绑定记录优先复用 ──
                    // 完整绑定记录（含掉线/未进 MCP 等待的通道）；只有勾选复用且确认进入 check_messages 等待态后才复用。
                    const bindingRecordChannelIds = Array.from(bindingRecordByChannel.keys())
                        .map(id => String(id || '').trim())
                        .filter(Boolean);
                    const bindingRecordCount = bindingRecordChannelIds.length;
                    const prioritizedBindingTargetChannelIds = shouldReuseBindings
                        ? bindingRecordChannelIds.slice().sort(compareChannelIds)
                        : [];
                    const targetChannelIds = [];
                    const targetChannelIdSet = new Set();
                    const skippedOccupiedTargetChannelIds = [];
                    const addTargetChannelId = (channelId) => {
                        const ch = String(channelId || '').trim();
                        if (!ch || targetChannelIdSet.has(ch) || targetChannelIds.length >= requestedSessionCount)
                            return;
                        const occupiedTarget = getOccupiedTarget(ch);
                        if (occupiedTarget) {
                            if (!skippedOccupiedTargetChannelIds.includes(ch)) {
                                skippedOccupiedTargetChannelIds.push(ch);
                                this._logBatchRetry('target channel skipped: already mcp waiting', {
                                    runId,
                                    channelId: ch,
                                    reason: occupiedTarget.reason,
                                    hasBinding: !!occupiedTarget.binding,
                                    composerId: String(occupiedTarget.binding?.composerId || ''),
                                    probe: occupiedTarget.probe
                                });
                            }
                            return;
                        }
                        targetChannelIdSet.add(ch);
                        targetChannelIds.push(ch);
                    };
                    for (const channelId of prioritizedBindingTargetChannelIds) {
                        addTargetChannelId(channelId);
                    }
                    for (const channelId of Array.from(rawByChannel.keys()).sort(compareChannelIds)) {
                        addTargetChannelId(channelId);
                    }
                    for (let i = 1; targetChannelIds.length < requestedSessionCount; i++) {
                        addTargetChannelId(String(i));
                    }
                    const waitingBindingChannelIds = new Set(bindingRecordChannelIds.filter(ch => boundChannelIds.has(ch) || isBindingWaitingActive(bindingRecordByChannel.get(ch)) || !!getOccupiedTarget(ch)));
                    const reusableBindingChannelIds = shouldReuseBindings
                        ? targetChannelIds.filter(ch => isUsableBinding(bindingRecordByChannel.get(ch))).sort(compareChannelIds)
                        : [];
                    const readyBoundBindingChannelIds = shouldReuseBindings
                        ? targetChannelIds
                            .filter(ch => !reusableBindingChannelIds.includes(ch))
                            .filter(ch => isReadyBoundBinding(bindingRecordByChannel.get(ch)))
                            .sort(compareChannelIds)
                        : [];
                    const pendingReuseBindingChannelIds = shouldReuseBindings
                        ? targetChannelIds
                            .filter(ch => bindingRecordByChannel.has(ch))
                            .filter(ch => !reusableBindingChannelIds.includes(ch))
                            .filter(ch => !readyBoundBindingChannelIds.includes(ch))
                            .sort(compareChannelIds)
                        : [];
                    const reservedBindingChannelIds = shouldReuseBindings
                        ? new Set(waitingBindingChannelIds)
                        : new Set(bindingRecordChannelIds);
                    const skippedBindingChannelIds = !shouldReuseBindings
                        ? bindingRecordChannelIds.slice().sort(compareChannelIds)
                        : [];
                    const effectiveTarget = requestedSessionCount;
                    const needToStart = Math.max(0, effectiveTarget);
                    const maxPendingReuseSlots = Math.max(0, needToStart - reusableBindingChannelIds.length - readyBoundBindingChannelIds.length);
                    const pendingReuseTargetChannelIds = pendingReuseBindingChannelIds.slice(0, maxPendingReuseSlots);
                    const pendingReuseTargetSet = new Set(pendingReuseTargetChannelIds);
                    const immediateUnboundTarget = Math.max(0, needToStart - pendingReuseTargetChannelIds.length);
                    // 复用绑定时，绑定数/目标超过当前通道总数才自动增长通道。
                    const maxBindingChannelId = bindingRecordChannelIds.reduce((max, id) => Math.max(max, Number(id) || 0), 0);
                    const ensureChannelTarget = Math.max(effectiveTarget, shouldReuseBindings ? maxBindingChannelId : 0, configuredCount);
                    this._logBatchRetry('binding records resolved', {
                        runId,
                        requestedSessionCount,
                        shouldReuseBindings,
                        bindingRecordCount,
                        bindingRecordChannelIds,
                        prioritizedBindingTargetChannelIds,
                        runtimeOccupiedChannelIds: Array.from(runtimeOccupiedByChannel.keys()).sort(compareChannelIds),
                        targetChannelIds,
                        skippedOccupiedTargetChannelIds,
                        waitingBindingChannelIds: [...waitingBindingChannelIds],
                        reusableBindingChannelIds,
                        readyBoundBindingChannelIds,
                        pendingReuseBindingChannelIds,
                        pendingReuseTargetChannelIds,
                        skippedBindingChannelIds,
                        effectiveTarget,
                        needToStart,
                        immediateUnboundTarget,
                        ensureChannelTarget,
                        configuredCount
                    });
                    if (ensureChannelTarget > configuredCount) {
                        void this._ensureBatchRetryChannelCount(ensureChannelTarget);
                    }
                    const usedChannelIds = new Set();
                    const initialSessions = [];
                    const addInitialSession = (channelId, binding) => {
                        const ch = String(channelId || '').trim();
                        if (!ch || usedChannelIds.has(ch) || initialSessions.length >= needToStart)
                            return;
                        usedChannelIds.add(ch);
                        const session = makeSession(ch, binding);
                        initialSessions.push(session);
                        this._logBatchRetry('initial session selected', {
                            runId,
                            channelId: ch,
                            sessionId: session.sessionId,
                            name: session.name,
                            hasBinding: !!binding,
                            reusedComposerId: binding ? String(binding.composerId || '') : ''
                        });
                    };
                    // 步骤 A：仅在用户勾选“复用已绑定通道”时，复用已确认进入 MCP 等待态的绑定通道。
                    for (const channelId of reusableBindingChannelIds) {
                        if (initialSessions.length >= needToStart)
                            break;
                        addInitialSession(channelId, bindingRecordByChannel.get(channelId));
                    }
                    for (const channelId of readyBoundBindingChannelIds) {
                        if (initialSessions.length >= needToStart)
                            break;
                        addInitialSession(channelId, bindingRecordByChannel.get(channelId));
                    }
                    // 步骤 B：未复用绑定或复用后仍不足目标数，用空闲未绑定通道补足。
                    const unboundInitialCandidates = shouldReuseBindings ? targetChannelIds : configuredChannelIds;
                    for (const channelId of unboundInitialCandidates) {
                        if (initialSessions.length >= immediateUnboundTarget)
                            break;
                        if (bindingRecordByChannel.has(channelId))
                            continue;
                        const releasedByCheckMessages = releasedChannelIds.has(channelId);
                        const probe = this._probeBatchRetryChannel(channelId, releasedByCheckMessages);
                        const used = usedChannelIds.has(channelId);
                        const bound = boundChannelIds.has(channelId);
                        const runtimeOccupied = runtimeOccupiedByChannel.has(channelId);
                        const idle = probe.idle;
                        const selected = !used && !bound && !runtimeOccupied && idle;
                        this._logBatchRetry('initial channel candidate', {
                            runId,
                            channelId,
                            selected,
                            used,
                            bound,
                            runtimeOccupied,
                            idle,
                            releasedByCheckMessages,
                            probe
                        });
                        if (used)
                            continue;
                        if (bound)
                            continue;
                        if (runtimeOccupied)
                            continue;
                        if (idle) {
                            addInitialSession(channelId);
                        }
                    }
                    const pendingChannelIds = [];
                    const addPendingChannelId = (channelId) => {
                        const ch = String(channelId || '').trim();
                        const releasedByCheckMessages = releasedChannelIds.has(ch);
                        const probe = ch ? this._probeBatchRetryChannel(ch, releasedByCheckMessages) : null;
                        const isBoundRecord = bindingRecordByChannel.has(ch);
                        const runtimeOccupied = runtimeOccupiedByChannel.has(ch);
                        const waitForBoundReuse = shouldReuseBindings && pendingReuseTargetSet.has(ch);
                        const waitForTargetChannel = shouldReuseBindings && targetChannelIdSet.has(ch);
                        const maxPendingReached = pendingChannelIds.length >= needToStart - initialSessions.length;
                        const hardReason = !ch ? 'empty'
                            : usedChannelIds.has(ch) ? 'already-used'
                                : pendingChannelIds.includes(ch) ? 'already-pending'
                                    : maxPendingReached ? 'max-pending-reached'
                                        : '';
                        const reason = hardReason
                            || (!shouldReuseBindings && isBoundRecord ? 'binding-reserved'
                                : waitingBindingChannelIds.has(ch) ? 'binding-waiting'
                                    : runtimeOccupied ? 'runtime-occupied'
                                        : probe?.effectiveWaitingActive ? 'runtime-waiting-active'
                                            : '');
                        if (hardReason || (reason && !waitForBoundReuse && !waitForTargetChannel)) {
                            this._logBatchRetry('pending channel candidate skipped', {
                                runId,
                                channelId: ch,
                                reason,
                                releasedByCheckMessages,
                                probe,
                                runtimeOccupied,
                                pendingChannelIds: [...pendingChannelIds]
                            });
                            return;
                        }
                        pendingChannelIds.push(ch);
                        this._logBatchRetry('pending channel candidate added', {
                            runId,
                            channelId: ch,
                            releasedByCheckMessages,
                            probe,
                            runtimeOccupied,
                            pendingChannelIds: [...pendingChannelIds]
                        });
                    };
                    const pendingCandidateCount = Math.max(configuredCount + needToStart, needToStart);
                    if (shouldReuseBindings) {
                        for (const channelId of targetChannelIds)
                            addPendingChannelId(channelId);
                    }
                    else {
                        for (const channelId of pendingReuseTargetChannelIds)
                            addPendingChannelId(channelId);
                        for (let i = 1; i <= pendingCandidateCount; i++)
                            addPendingChannelId(String(i));
                        for (const channelId of Array.from(rawByChannel.keys()).sort(compareChannelIds))
                            addPendingChannelId(channelId);
                    }
                    if (pendingChannelIds.length > 0) {
                        const maxPendingId = pendingChannelIds.reduce((max, id) => Math.max(max, Number(id) || 0), ensureChannelTarget);
                        this._logBatchRetry('pending channel count ensure scheduled', {
                            runId,
                            maxPendingId,
                            pendingChannelIds,
                            configuredCount
                        });
                        void this._ensureBatchRetryChannelCount(maxPendingId);
                    }
                    const effectiveOpts = opts;
                    this._logBatchRetry('effective opts resolved', {
                        runId,
                        shouldReuseBindings,
                        reuseExisting: effectiveOpts.reuseExisting,
                        optsReuseExisting: opts.reuseExisting
                    });
                    const sessionStore = [];
                    const storeSessions = (nextSessions) => {
                        for (const session of nextSessions) {
                            const existing = sessionStore.find(item => item.sessionId === session.sessionId);
                            if (existing)
                                Object.assign(existing, session);
                            else
                                sessionStore.push(session);
                        }
                    };
                    const onUpdate = (status) => {
                        this._recordBatchRetryHistory(status);
                        this._notifyBatchRetrySuccess(status);
                        if (status.done) {
                            const pendingRun = this._batchRetryPendingRun;
                            if (pendingRun && pendingRun.started < pendingRun.requested) {
                                const channelWait = this._makeBatchRetryChannelWaitStatus(pendingRun);
                                try {
                                    webviewView.webview.postMessage({
                                        command: 'batchRetryStatus',
                                        data: {
                                            ...status,
                                            text: channelWait.message,
                                            done: false,
                                            channelWait
                                        }
                                    });
                                }
                                catch { }
                                return;
                            }
                            this._clearBatchRetryPendingRun(false);
                        }
                        try {
                            webviewView.webview.postMessage({ command: 'batchRetryStatus', data: status });
                        }
                        catch { }
                    };
                    const launchCallback = async (sid) => {
                        const session = sessionStore.find(s => s.sessionId === sid);
                        return engine.silentLaunchSession(sid, session?.joinPhrase || '', session?.name || sid, {
                            reuseExisting: false,
                            channelId: session?.channelId || '',
                            visibleSubmit: !opts.useComposerBridge
                        });
                    };
                    const onlineCheck = (_sid) => false;
                    let batchStarted = false;
                    let operationQueue = Promise.resolve();
                    const startBatch = (sessions) => {
                        storeSessions(sessions);
                        batchStarted = true;
                        operationQueue = engine.startBatchRetry(sessions, effectiveOpts, onUpdate, launchCallback, onlineCheck)
                            .catch((err) => {
                            try {
                                webviewView.webview.postMessage({
                                    command: 'batchRetryStatus',
                                    data: {
                                        text: '批量重试启动失败: ' + (err instanceof Error ? err.message : String(err)),
                                        done: true,
                                        error: true,
                                        tasks: [],
                                        sessionComposerBindings: [],
                                        count: '0/0'
                                    }
                                });
                            }
                            catch { }
                        });
                    };
                    const appendOrStart = async (sessions) => {
                        if (sessions.length === 0)
                            return 0;
                        storeSessions(sessions);
                        const op = async () => {
                            if (!batchStarted || !engine.isRunning()) {
                                batchStarted = true;
                                await engine.startBatchRetry(sessions, effectiveOpts, onUpdate, launchCallback, onlineCheck);
                                return sessions.length;
                            }
                            const result = await engine.appendSessions(sessions);
                            return result.ok ? result.appended : 0;
                        };
                        const nextQueue = operationQueue.then(op, op).catch((err) => {
                            try {
                                webviewView.webview.postMessage({
                                    command: 'batchRetryStatus',
                                    data: {
                                        text: '追加批量重试通道失败: ' + (err instanceof Error ? err.message : String(err)),
                                        done: false,
                                        error: true,
                                        tasks: [],
                                        sessionComposerBindings: [],
                                        count: '0/0'
                                    }
                                });
                            }
                            catch { }
                            return 0;
                        });
                        operationQueue = nextQueue;
                        return nextQueue;
                    };
                    if (initialSessions.length > 0) {
                        initialSessions.sort((a, b) => compareChannelIds(String(a.channelId || ''), String(b.channelId || '')));
                        this._logBatchRetry('initial batch start', {
                            runId,
                            sessions: initialSessions.map(session => ({
                                sessionId: session.sessionId,
                                channelId: session.channelId,
                                name: session.name
                            }))
                        });
                        startBatch(initialSessions);
                    }
                    const scheduledBindingChannelIds = shouldReuseBindings
                        ? bindingRecordChannelIds.filter(channelId => !pendingReuseTargetSet.has(channelId))
                        : Array.from(reservedBindingChannelIds);
                    const scheduledOccupiedChannelIds = Array.from(runtimeOccupiedByChannel.keys());
                    if (pendingChannelIds.length > 0) {
                        this._logBatchRetry('initial pending run create', {
                            runId,
                            requested: needToStart,
                            started: initialSessions.length,
                            pendingChannelIds,
                            pendingReuseBindingChannelIds,
                            scheduledChannelIds: [...usedChannelIds, ...scheduledBindingChannelIds, ...scheduledOccupiedChannelIds],
                            boundChannelIds: [...boundChannelIds],
                            scheduledOccupiedChannelIds,
                            waitingBindingChannelIds: [...waitingBindingChannelIds],
                            usedChannelIds: [...usedChannelIds],
                            reuseBindingByChannel: Array.from(bindingRecordByChannel.entries())
                                .filter(([channelId]) => pendingReuseTargetSet.has(channelId))
                                .map(([channelId, binding]) => ({
                                channelId,
                                composerId: String(binding?.composerId || ''),
                                clientId: String(binding?.clientId || ''),
                                clientMatches: binding?.clientMatches === true,
                                exists: binding?.exists === true
                            }))
                        });
                        this._startBatchRetryPendingRun({
                            id: runId,
                            requested: needToStart,
                            started: initialSessions.length,
                            pendingChannelIds: new Set(pendingChannelIds),
                            addedChannelIds: new Set(),
                            scheduledChannelIds: new Set([
                                ...usedChannelIds,
                                ...scheduledBindingChannelIds,
                                ...scheduledOccupiedChannelIds
                            ]),
                            reuseBindingChannelIds: pendingReuseTargetSet,
                            reuseBindingByChannel: new Map(Array.from(bindingRecordByChannel.entries())
                                .filter(([channelId]) => pendingReuseTargetSet.has(channelId))),
                            allowedChannelIds: shouldReuseBindings ? new Set(targetChannelIds) : undefined,
                            targetClientId: focusedTargetClientId,
                            targetClientSeenAfter: focusedTargetClientSeenAfter,
                            makeSession: (channelId, binding) => makeSession(channelId, binding),
                            append: appendOrStart
                        });
                    }
                    if (initialSessions.length === 0 && pendingChannelIds.length === 0) {
                        this._logBatchRetry('batch retry no usable or pending channels', {
                            runId,
                            requestedSessionCount,
                            configuredCount,
                            boundChannelIds: [...boundChannelIds],
                            usedChannelIds: [...usedChannelIds],
                            probes: configuredChannelIds.map(channelId => this._probeBatchRetryChannel(channelId))
                        });
                        webviewView.webview.postMessage({
                            command: 'batchRetryStatus',
                            data: {
                                text: '当前没有可用通道，请先打开 MCP 通道',
                                done: false,
                                tasks: [],
                                sessionComposerBindings: [],
                                count: '0/0'
                            }
                        });
                    }
                    break;
                }
                case 'batchRetryStop': {
                    const engine = this._batchRetryEngine;
                    const wasEngineRunning = !!engine?.isRunning();
                    const clearedPendingRun = this._clearBatchRetryPendingRun();
                    engine?.stopAll();
                    if (clearedPendingRun && !wasEngineRunning) {
                        webviewView.webview.postMessage({
                            command: 'batchRetryStatus',
                            data: {
                                text: '已停止批量重试',
                                done: true,
                                tasks: [],
                                sessionComposerBindings: [],
                                count: '0/0',
                                clientCount: engine?.getOnlineClientIds().length || 0,
                                parallelWindowDispatch: true
                            }
                        });
                    }
                    break;
                }
                case 'batchRetryRevealChannel': {
                    const channelId = String(msg.channelId || '').trim();
                    const result = await this._revealBatchRetryChannel(channelId);
                    webviewView.webview.postMessage({
                        command: 'batchRetryRevealChannelResult',
                        data: result
                    });
                    if (result?.ok) {
                        const launched = this._batchRetryEngine?.getLaunchedSessions() || [];
                        webviewView.webview.postMessage({ command: 'batchRetryLaunchedSessions', data: launched });
                        const bindings = await this._retryFocusedBatchBindingOp('reveal_channel_bindings', (opts) => this._batchRetryEngine?.listChannelBindings(8000, opts)).catch(() => null);
                        if (bindings) {
                            webviewView.webview.postMessage({
                                command: 'batchRetryChannelBindings',
                                data: bindings
                            });
                        }
                    }
                    break;
                }
                case 'batchRetryStopTask': {
                    this._batchRetryEngine?.stopTask(String(msg.taskId || ''));
                    break;
                }
                case 'batchRetryClearTask': {
                    this._batchRetryEngine?.clearTask(String(msg.taskId || ''));
                    break;
                }
                case 'batchRetryWebviewLog': {
                    this._logBatchRetry('webview event', msg.data || {});
                    break;
                }
                case 'batchRetryRevealTask': {
                    const engine = this._batchRetryEngine;
                    const taskId = String(msg.taskId || '');
                    const channelId = String(msg.channelId || '').trim();
                    const startedAt = Date.now();
                    let result = { ok: false, error: 'engine_not_ready' };
                    this._logBatchRetry('reveal handler received', {
                        taskId,
                        channelId,
                        hasEngine: !!engine
                    });
                    if (engine) {
                        const onlineClientIds = engine.getOnlineClientIds();
                        this._logBatchRetry('reveal in new agent begin', {
                            taskId,
                            channelId,
                            onlineClientIds,
                            timeoutMs: 12000
                        });
                        result = await engine.revealTaskComposer(taskId, 12000);
                        this._logBatchRetry('reveal in new agent result', {
                            taskId,
                            channelId,
                            result,
                            elapsedMs: Date.now() - startedAt
                        });
                    }
                    this._logBatchRetry('reveal handler post result', {
                        taskId,
                        channelId,
                        result,
                        elapsedMs: Date.now() - startedAt
                    });
                    webviewView.webview.postMessage({
                        command: 'batchRetryRevealTaskResult',
                        data: { ...result, taskId, channelId }
                    });
                    break;
                }
                case 'batchRetryRevealComposer': {
                    const engine = this._batchRetryEngine;
                    const composerId = String(msg.composerId || '').trim();
                    const channelId = String(msg.channelId || '').trim();
                    const clientId = String(msg.clientId || '').trim();
                    const startedAt = Date.now();
                    let result = { ok: false, error: 'engine_not_ready' };
                    this._logBatchRetry('reveal composer handler received', {
                        composerId,
                        channelId,
                        clientId,
                        hasEngine: !!engine
                    });
                    if (engine) {
                        const onlineClientIds = engine.getOnlineClientIds();
                        const targetClientIds = clientId ? [clientId] : undefined;
                        this._logBatchRetry('reveal composer in new agent begin', {
                            composerId,
                            channelId,
                            clientId,
                            onlineClientIds,
                            targetClientIds,
                            timeoutMs: 12000
                        });
                        result = await engine.revealComposerInNewAgent(composerId, 12000, targetClientIds, { channelId, preferredClientId: clientId });
                        this._logBatchRetry('reveal composer in new agent result', {
                            composerId,
                            channelId,
                            clientId,
                            result,
                            elapsedMs: Date.now() - startedAt
                        });
                    }
                    webviewView.webview.postMessage({
                        command: 'batchRetryRevealComposerResult',
                        data: { ...result, composerId, channelId, clientId }
                    });
                    break;
                }
                case 'batchRetrySingleTask': {
                    const result = this._batchRetryEngine?.retrySingleTask(String(msg.taskId || ''), String(msg.sessionId || ''), msg.composerId ? String(msg.composerId) : undefined);
                    webviewView.webview.postMessage({ command: 'batchRetrySingleTaskResult', data: result || { ok: false, error: 'engine_not_ready' } });
                    break;
                }
                case 'batchRetryRestartChannel': {
                    const channelId = String(msg.channelId || '').trim();
                    const result = await this._restartBatchRetryChannel(channelId, webviewView.webview);
                    webviewView.webview.postMessage({
                        command: 'batchRetryRestartChannelResult',
                        data: result
                    });
                    if (result?.ok) {
                        const launched = this._batchRetryEngine?.getLaunchedSessions() || [];
                        webviewView.webview.postMessage({ command: 'batchRetryLaunchedSessions', data: launched });
                    }
                    break;
                }
                case 'batchRetryGetLaunchedSessions': {
                    const launched = this._batchRetryEngine?.getLaunchedSessions() || [];
                    webviewView.webview.postMessage({ command: 'batchRetryLaunchedSessions', data: launched });
                    break;
                }
                case 'batchRetryGetChannelBindings': {
                    const engine = this._batchRetryEngine;
                    const result = await this._retryFocusedBatchBindingOp('get_channel_bindings', (opts) => engine?.listChannelBindings(8000, opts));
                    webviewView.webview.postMessage({
                        command: 'batchRetryChannelBindings',
                        data: result || { ok: false, bindings: [], error: 'engine_not_ready' }
                    });
                    break;
                }
                case 'batchRetryBindActiveChannel': {
                    const engine = this._batchRetryEngine;
                    const result = await this._retryFocusedBatchBindingOp('bind_active_channel', (opts) => engine?.bindActiveComposerToChannel(String(msg.channelId || ''), 8000, opts));
                    webviewView.webview.postMessage({
                        command: 'batchRetryBindActiveChannelResult',
                        data: result || { ok: false, error: 'engine_not_ready' }
                    });
                    break;
                }
                case 'batchRetryClearChannelBinding': {
                    const engine = this._batchRetryEngine;
                    const result = await this._retryFocusedBatchBindingOp('clear_channel_binding', (opts) => engine?.clearChannelBinding(String(msg.channelId || ''), msg.composerId ? String(msg.composerId) : undefined, 8000, opts));
                    webviewView.webview.postMessage({
                        command: 'batchRetryClearChannelBindingResult',
                        data: result || { ok: false, error: 'engine_not_ready' }
                    });
                    break;
                }
                case 'batchRetryWatchToggle': {
                    const engine = this._batchRetryEngine;
                    if (engine) {
                        const current = engine.getWatchedSids();
                        const sid = String(msg.sid || '');
                        const watched = !!msg.watched;
                        const updated = watched
                            ? [...new Set([...current, sid])]
                            : current.filter(s => s !== sid);
                        engine.setWatchedSids(updated, { throttleMs: 3000 });
                        webviewView.webview.postMessage({ command: 'batchRetryWatchToggleResult', data: { sid, watched, ok: true } });
                    }
                    break;
                }
            }
        });
    }
    showUpdateNotice(payload) {
        this.focusPanel();
        const push = () => {
            this._view?.webview.postMessage({
                command: 'showUpdateNotice',
                data: payload
            });
        };
        try {
            push();
        }
        catch { }
        setTimeout(() => {
            try {
                push();
            }
            catch { }
        }, 200);
        setTimeout(() => {
            try {
                push();
            }
            catch { }
        }, 600);
    }
    closeUpdateNotice(noticeId) {
        const push = () => {
            this._view?.webview.postMessage({
                command: 'hideUpdateNotice',
                noticeId
            });
        };
        try {
            push();
        }
        catch { }
    }
    sendStatus(status) {
        this._view?.webview.postMessage({ command: 'status', data: status });
    }
    pushSeamlessStatus() {
        this._view?.webview.postMessage(buildSeamlessStatusPayload());
    }
    // 解析通道当前绑定的 composerId（用于读取 Cursor 原生对话做高保真上下文转移）
    async _resolveChannelComposerId(channelId) {
        const engine = this._batchRetryEngine;
        if (!engine)
            return '';
        try {
            const r = await engine.listChannelBindings(2000);
            const all = [...(r.bindings || []), ...(r.allBindings || [])];
            const match = all.find(b => String(b?.channelId || '').trim() === String(channelId).trim() &&
                String(b?.composerId || '').trim());
            return match ? String(match.composerId).trim() : '';
        }
        catch {
            return '';
        }
    }
    focusPanel() {
        if (this._view) {
            this._view.show(true);
        }
        else {
            vscode.commands.executeCommand('qingtian.panel.focus');
        }
    }
    postMessage(msg) {
        this._view?.webview.postMessage(msg);
    }
    _cleanupStalePasteImages() {
        const now = Date.now();
        for (const [id, pending] of this._pendingPasteImages.entries()) {
            if (now - pending.createdAt > PASTE_IMAGE_CHUNK_TTL_MS) {
                this._pendingPasteImages.delete(id);
            }
        }
    }
    _savePastedImage(webview, dataUrl, mimeHint = '') {
        const matches = dataUrl.match(/^data:(.+);base64,(.+)$/);
        if (!matches) {
            throw new Error('Invalid image data');
        }
        fs.mkdirSync(PASTE_IMAGE_TMP_DIR, { recursive: true });
        const imageData = Buffer.from(matches[2], 'base64');
        const mimeType = String(mimeHint || matches[1] || 'image/png');
        const ext = this._mimeToExtension(mimeType);
        const filename = `paste_${Date.now()}_${Math.random().toString(36).slice(2, 7)}.${ext}`;
        const tmpPath = path.join(PASTE_IMAGE_TMP_DIR, filename);
        fs.writeFileSync(tmpPath, imageData);
        webview.postMessage({
            command: 'filesSelected',
            data: [{
                    path: tmpPath,
                    name: tmpPath,
                    displayName: filename,
                    type: 'image',
                    previewSrc: webview.asWebviewUri(vscode.Uri.file(tmpPath)).toString()
                }]
        });
        console.log(`[QingTian] pasted image saved: ${tmpPath} (${imageData.length} bytes)`);
    }
    _mimeToExtension(mimeType) {
        const normalized = mimeType.toLowerCase().trim();
        const map = {
            'image/png': 'png',
            'image/jpeg': 'jpg',
            'image/jpg': 'jpg',
            'image/gif': 'gif',
            'image/webp': 'webp',
            'image/bmp': 'bmp',
            'image/tiff': 'tiff',
            'image/heic': 'heic',
            'image/heif': 'heif',
            'image/avif': 'avif',
            'image/svg+xml': 'svg'
        };
        return map[normalized] || 'png';
    }
    _getFileType(filePath) {
        const ext = path.extname(filePath).toLowerCase().replace('.', '');
        if (['png', 'jpg', 'jpeg', 'gif', 'webp', 'svg', 'bmp', 'ico', 'heic', 'heif', 'tif', 'tiff', 'avif'].includes(ext)) {
            return 'image';
        }
        return 'file';
    }
    _getHtmlContent(webview) {
        const cssUri = webview.asWebviewUri(vscode.Uri.joinPath(this._extensionUri, 'resources', 'webview.css'));
        const jsUri = webview.asWebviewUri(vscode.Uri.joinPath(this._extensionUri, 'resources', 'webview.js'));
        const version = this._getDisplayVersion();
        const channelCount = (0, mcpServer_1.getChannelCount)();
        const workspaceFolders = vscode.workspace.workspaceFolders;
        const workspaceName = workspaceFolders?.[0]?.name || '未打开工作区';
        const bridgeScriptPath = vscode.Uri.joinPath(this._extensionUri, 'bridge', 'telegram.mjs').fsPath.replace(/\\/g, '/');
        // 新手引导图片资源 URI（缺图时 webview 显示占位框）
        const obImg = (name) => webview.asWebviewUri(vscode.Uri.joinPath(this._extensionUri, 'resources', 'onboarding', name)).toString();
        const onboardingImages = JSON.stringify({
            'auto-model': obImg('auto-model.png'),
            'composer-input': obImg('composer-input.png'),
            'running-check': obImg('running-check.png'),
            'advanced-model': obImg('advanced-model.png')
        }).replace(/'/g, '&#39;');
        return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="${cssUri}">
    <title>SlashSubs</title>
    <style>
    .crypto-method-card {
      display: flex; flex-direction: column; align-items: center; justify-content: center;
      gap: 4px; padding: 10px 4px; border-radius: 10px; cursor: pointer;
      border: 1px solid rgba(130,154,181,0.18); background: rgba(255,255,255,0.06);
      transition: all .15s ease; min-height: 64px; color: #fff;
    }
    .crypto-method-card:hover {
      background: rgba(255,255,255,0.14); border-color: rgba(130,154,181,0.35);
      transform: translateY(-1px); box-shadow: 0 2px 8px rgba(0,0,0,0.08);
    }
    .crypto-method-card:active { transform: scale(0.97); }
    .crypto-method-icon { font-size: 22px; line-height: 1; }
    .crypto-method-label { font-size: 11px; font-weight: 600; color: #fff; text-align: center; line-height: 1.2; }
    #crypto-pay-details { animation: fadeSlideUp .2s ease; }
    @keyframes fadeSlideUp { from { opacity: 0; transform: translateY(6px); } to { opacity: 1; transform: translateY(0); } }
    #crypto-pay-details { background: rgba(30,30,30,0.95) !important; border-color: rgba(255,255,255,0.1) !important; }
    #crypto-pay-details * { color: #e0e0e0; }
    #crypto-pay-amount { color: #fff !important; font-weight: 700; }
    #crypto-pay-address { background: rgba(255,255,255,0.06) !important; color: #ccc !important; }
    #crypto-pay-method-name { color: #fff !important; }
    #btn-crypto-back { color: #999 !important; }
    #crypto-pay-network-badge { background: #1a1a2e !important; color: #fff !important; }
    .license-gate, .license-gate * { color: #fff; }
    .license-desc { color: #aaa !important; }
    #crypto-pay-warning { color: #888 !important; }
    </style>
</head>
<body data-theme="light" data-lang="en" data-send-mode="${this._initialSendMode}" data-bridge-script="${bridgeScriptPath}" data-onboarding-images='${onboardingImages}'>
    <div id="license-gate" class="license-gate">
        <div class="license-logo">SlashSubs</div>
        <p class="license-desc" id="license-desc">SlashSubs v${version} — free trial for new users</p>

        <!-- Step 0: Plan picker -->
        <div id="license-paywall" class="license-form" style="display:flex;flex-direction:column;gap:10px;align-items:center;max-width:480px;margin:0 auto;">
            <div id="license-plan-picker" style="display:flex;gap:8px;width:100%;max-width:360px;">
                <button type="button" class="primary-button license-btn" id="btn-plan-monthly" data-plan="monthly" style="flex:1;">Monthly<br/><b>$9.9</b></button>
                <button type="button" class="license-purchase-link purchase-link" id="btn-plan-yearly" data-plan="yearly" style="flex:1;border:1px solid rgba(255,255,255,.25);padding:10px;border-radius:8px;">Yearly<br/><b>$49</b></button>
            </div>

            <!-- Step 1: Crypto method picker (card grid) -->
            <div id="crypto-method-grid" style="display:grid;grid-template-columns:repeat(3,1fr);gap:8px;width:100%;margin-top:8px;">
                <button type="button" class="crypto-method-card" data-method="btc">
                    <span class="crypto-method-icon" style="color:#f7931a;">₿</span>
                    <span class="crypto-method-label">Bitcoin</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="eth">
                    <span class="crypto-method-icon" style="color:#627eea;">Ξ</span>
                    <span class="crypto-method-label">Ethereum</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdt-trc20">
                    <span class="crypto-method-icon" style="color:#26a17b;">₮</span>
                    <span class="crypto-method-label">USDT (TRC20)</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdt-erc20">
                    <span class="crypto-method-icon" style="color:#26a17b;">₮</span>
                    <span class="crypto-method-label">USDT (ERC20)</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdt-arbitrum">
                    <span class="crypto-method-icon" style="color:#26a17b;">₮</span>
                    <span class="crypto-method-label">USDT (Arbitrum)</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdt-polygon">
                    <span class="crypto-method-icon" style="color:#26a17b;">₮</span>
                    <span class="crypto-method-label">USDT (Polygon)</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdt-solana">
                    <span class="crypto-method-icon" style="color:#26a17b;">₮</span>
                    <span class="crypto-method-label">USDT (Solana)</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdc">
                    <span class="crypto-method-icon" style="color:#2775ca;">$</span>
                    <span class="crypto-method-label">USDC</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdc-arbitrum">
                    <span class="crypto-method-icon" style="color:#2775ca;">$</span>
                    <span class="crypto-method-label">USDC (Arbitrum)</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdc-polygon">
                    <span class="crypto-method-icon" style="color:#2775ca;">$</span>
                    <span class="crypto-method-label">USDC (Polygon)</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="usdc-solana">
                    <span class="crypto-method-icon" style="color:#2775ca;">$</span>
                    <span class="crypto-method-label">USDC (Solana)</span>
                </button>
                <button type="button" class="crypto-method-card" data-method="binance-pay">
                    <span class="crypto-method-icon" style="color:#f0b90b;">◆</span>
                    <span class="crypto-method-label">Binance Pay</span>
                </button>
            </div>

            <!-- Step 2: Payment details (hidden by default) -->
            <div id="crypto-pay-details" style="display:none;width:100%;max-width:380px;background:rgba(255,255,255,0.95);border-radius:16px;padding:20px;border:1px solid rgba(130,154,181,0.18);margin-top:4px;">
                <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;">
                    <button type="button" id="btn-crypto-back" style="background:none;border:none;cursor:pointer;font-size:14px;color:#666;padding:4px 8px;">← Back</button>
                    <span id="crypto-pay-method-name" style="font-weight:700;font-size:14px;color:#333;"></span>
                    <span></span>
                </div>
                <div style="text-align:center;margin-bottom:10px;">
                    <div style="font-size:12px;color:#666;">Total</div>
                    <div id="crypto-pay-amount" style="font-size:22px;font-weight:700;color:#111;"></div>
                </div>
                <div style="text-align:center;margin-bottom:6px;">
                    <div id="crypto-pay-network-badge" style="display:inline-block;background:#1a1a2e;color:#fff;font-size:11px;font-weight:600;padding:4px 12px;border-radius:6px;margin-bottom:8px;"></div>
                </div>
                <div style="background:rgba(0,0,0,0.03);border-radius:10px;padding:10px;margin-bottom:12px;word-break:break-all;font-family:monospace;font-size:12px;text-align:center;user-select:all;cursor:text;" id="crypto-pay-address"></div>
                <div style="font-size:11px;color:#888;text-align:center;margin-bottom:12px;" id="crypto-pay-warning"></div>
                <div style="text-align:center;margin-bottom:12px;">
                    <img id="crypto-pay-qr" alt="Payment QR" style="width:180px;height:180px;background:#fff;border-radius:8px;object-fit:contain;" />
                </div>
                <div style="display:flex;gap:8px;flex-direction:column;">
                    <button type="button" class="primary-button license-btn" id="btn-verify-payment" style="width:100%;">Verify payment</button>
                    <button type="button" class="license-purchase-link purchase-link" id="btn-copy-pay-address" style="text-align:center;">Copy payment address</button>
                </div>
            </div>

            <button type="button" class="license-purchase-link purchase-link" id="btn-debug-unlock" style="opacity:.65;">QA: Simulate paid unlock</button>
            <input type="password" class="license-input" id="license-code-input" placeholder="Backup: renewal password (optional)" autocomplete="off" spellcheck="false" />
            <button type="button" class="license-purchase-link purchase-link" id="btn-activate">Renew with password</button>
            <button type="button" class="license-purchase-link purchase-link" id="btn-start-trial" style="display:none;">Start free trial</button>
        </div>
        <div id="license-feedback" class="license-feedback"></div>
        <div class="support-footer support-footer-compact">
            <div><a href="https://chat.whatsapp.com/EJUfSlyZxlQ0bYCtdYqsQA" id="license-whatsapp-link">WhatsApp Group</a></div>
        </div>
        <!-- Keep hidden elements for backward compat -->
        <img id="license-qr" alt="" style="display:none;" />
        <div id="license-pay-meta" style="display:none;"></div>
    </div>
    <div id="app" class="app-shell hidden">
        <section id="runtime-notice" class="runtime-notice hidden" data-level="warning">
            <div class="runtime-notice-copy">
                <div id="runtime-notice-title" class="runtime-notice-title">MCP 配置待刷新</div>
                <div id="runtime-notice-message" class="runtime-notice-message"></div>
            </div>
            <div class="runtime-notice-actions">
                <button id="btn-runtime-refresh" class="soft-button" type="button" title="会改写 mcp.json 并可能让 Cursor 重启 MCP（打断同会话）。正常使用请勿点。">刷新配置（慎用）</button>
                <button id="btn-runtime-reload" class="ghost-button runtime-reload-button" type="button">重载窗口</button>
            </div>
        </section>
        <div id="acct-toast" class="acct-toast hidden" role="status" aria-live="polite"></div>

        <div id="seamless-restart-modal" class="seamless-restart-modal hidden" aria-hidden="true">
            <div id="seamless-restart-overlay" class="seamless-restart-overlay"></div>
            <div class="seamless-restart-dialog" role="dialog" aria-modal="true" aria-labelledby="seamless-restart-title">
                <button id="btn-seamless-restart-close" class="seamless-restart-close" type="button" aria-label="关闭">×</button>
                <div class="seamless-restart-icon">!</div>
                <div id="seamless-restart-title" class="seamless-restart-title">账号接管注入已写入</div>
                <div class="seamless-restart-text">
                    请完全退出 Cursor，然后重新打开 Cursor，注入才会生效。不要只重载窗口，也不要只关闭当前面板。
                    <div class="seamless-restart-admin-tip">强烈建议<b>以管理员身份重启 Cursor</b>，否则注入可能被系统拦截、重启后仍显示黄点。</div>
                </div>
                <button id="btn-seamless-restart-ok" class="primary-button seamless-restart-ok" type="button">知道了，我手动重启</button>
            </div>
        </div>

        <div id="seamless-pending-warning-modal" class="seamless-restart-modal hidden" aria-hidden="true">
            <div id="seamless-pending-overlay" class="seamless-restart-overlay"></div>
            <div class="seamless-restart-dialog" role="dialog" aria-modal="true" aria-labelledby="seamless-pending-title">
                <button id="btn-seamless-pending-close" class="seamless-restart-close" type="button" aria-label="关闭">×</button>
                <div class="seamless-restart-icon seamless-pending-icon">!</div>
                <div id="seamless-pending-title" class="seamless-restart-title seamless-pending-title">注入未生效（黄点）</div>
                <div class="seamless-restart-text">
                    右上角是<b class="seamless-pending-em">黄点</b>：注入文件已写入，但没有在当前 Cursor 中生效。通常是上次没有用<b class="seamless-pending-em">管理员权限</b>重启 Cursor，导致注入被系统拦截。请按下面步骤处理：
                    <ol class="seamless-pending-steps">
                        <li>点击右上角圆点，<b class="seamless-pending-em">取消注入</b></li>
                        <li><b class="seamless-pending-em">完全退出 Cursor</b>（不是重载窗口）</li>
                        <li>右键 Cursor 图标，<b class="seamless-pending-em">以管理员身份运行</b></li>
                        <li>再次点击圆点，<b class="seamless-pending-em">重新注入</b></li>
                    </ol>
                </div>
                <div class="seamless-pending-actions">
                    <button id="btn-seamless-pending-cancel-inject" class="seamless-pending-danger-btn" type="button">去取消注入</button>
                    <button id="btn-seamless-pending-ok" class="ghost-button seamless-pending-ok" type="button">我知道了</button>
                </div>
            </div>
        </div>

        <div id="accounts-modal" class="accounts-modal hidden" aria-hidden="true">
            <div id="accounts-modal-overlay" class="accounts-modal-overlay"></div>
            <div class="accounts-modal-dialog" role="dialog" aria-modal="true" aria-labelledby="accounts-modal-title">
                <div class="accounts-modal-header">
                    <div>
                        <div class="accounts-modal-kicker">Cursor 账号管理</div>
                        <h2 id="accounts-modal-title">账号、用量与切号</h2>
                    </div>
                    <div class="accounts-modal-header-actions">
                        <button id="btn-import-accounts" class="account-header-btn" type="button" title="从剪贴板导入账号">导入账号</button>
                        <button id="btn-export-accounts" class="account-header-btn" type="button" title="导出账号到剪贴板">导出全部</button>
                    </div>
                    <button id="btn-close-accounts" class="icon-button accounts-modal-close" type="button" title="关闭账号管理" aria-label="关闭账号管理">×</button>
                </div>
                <div class="accounts-modal-toolbar">
                    <button id="btn-toggle-add-token" class="account-primary-btn" type="button">+ 添加账号</button>
                    <button id="btn-save-current" class="account-secondary-btn" type="button">保存当前账号</button>
                    <button id="btn-copy-token" class="account-secondary-btn" type="button">复制当前 Token</button>
                    <button id="btn-refresh-accounts" class="icon-button account-refresh-btn" type="button" title="刷新账号状态" aria-label="刷新账号状态" data-icon="refresh"></button>
                    <div class="accounts-toolbar-spacer"></div>
                    <label class="billing-select-all-label"><input type="checkbox" id="billing-select-all" /> 全选</label>
                    <button id="btn-billing-refresh" class="icon-button billing-toolbar-btn billing-toolbar-icon-btn" type="button" title="刷新账号用量，未勾选时刷新全部" aria-label="刷新账号用量" data-icon="refresh"></button>
                    <button id="btn-billing-export" class="billing-toolbar-btn" type="button" title="导出选中账号">导出选中</button>
                    <button id="btn-billing-delete" class="billing-toolbar-btn billing-toolbar-btn-danger" type="button" title="删除选中账号">删除选中</button>
                </div>
                <div class="accounts-search-row">
                    <span class="accounts-search-icon">⌕</span>
                    <input id="account-search-input" class="accounts-search-input" type="search" placeholder="搜索邮箱、名称、订阅计划..." />
                </div>
                <div class="accounts-table-wrap">
                    <div id="billing-list" class="billing-list account-table-list">
                        <div class="billing-empty">暂无账号，请先添加账号</div>
                    </div>
                </div>
            </div>
        </div>

        <div id="account-add-form" class="account-add-modal hidden" aria-hidden="true">
            <div id="account-add-overlay" class="account-add-overlay"></div>
            <div class="account-add-dialog" role="dialog" aria-modal="true" aria-labelledby="account-add-title">
                <div class="account-add-header">
                    <h3 id="account-add-title">添加 Cursor 账号</h3>
                    <button id="btn-cancel-add" class="ghost-button" type="button" title="关闭">×</button>
                </div>
                <div class="account-add-tabs">
                    <button id="btn-account-add-session" class="account-add-tab active" type="button" data-mode="session">Session Token</button>
                    <button id="btn-account-add-access" class="account-add-tab" type="button" data-mode="access">Access Token</button>
                </div>
                <div id="account-add-info" class="account-add-info">通过 WorkosCursorSessionToken 自动获取 Access Token，推荐使用此方式，支持后续刷新。</div>
                <label id="acct-name-row" class="account-add-field hidden">
                    <span>Email / 名称</span>
                    <input id="acct-name-input" class="acct-input" type="text" placeholder="Access Token 方式建议填写邮箱" />
                </label>
                <label class="account-add-field">
                    <span id="acct-token-label">Session Token</span>
                    <textarea id="acct-token-input" class="acct-input" rows="6" placeholder="请输入 WorkosCursorSessionToken..." style="resize:vertical"></textarea>
                </label>
                <div class="account-add-hint" id="account-add-hint">可通过浏览器控制台或 Cookie 管理插件获取 WorkosCursorSessionToken。</div>
                <div class="account-form-actions">
                    <button id="btn-add-token" class="account-primary-btn" type="button">添加账号</button>
                    <button id="btn-cancel-add-secondary" class="account-secondary-btn" type="button">取消</button>
                </div>
            </div>
        </div>
        <section class="hero-panel panel-card">
            <div class="hero-top">
                <div>
                    <div class="hero-brand-row">
                        <span class="hero-kicker">SLASHSUBS</span>
                        <a class="purchase-link" id="btn-whatsapp-link" href="https://chat.whatsapp.com/EJUfSlyZxlQ0bYCtdYqsQA" target="_blank" rel="noopener noreferrer">WhatsApp</a>
                    </div>
                    <div class="hero-title-row">
                        <h1 class="hero-title">Control Deck</h1>
                        <span class="hero-version">v${version}</span>
                    </div>
                </div>
                <div class="hero-actions">
                    <button id="btn-toggle-seamless-global" class="icon-button seamless-toggle off" type="button" title="账号接管注入：未启用">●</button>
                    <button id="btn-language-toggle" class="icon-button language-toggle" type="button" title="Switch to Chinese" aria-label="Switch to Chinese" aria-pressed="true" data-lang="en">
                        <svg class="language-toggle-icon" viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
                            <path d="m5 8 6 6"/>
                            <path d="m4 14 6-6 2-3"/>
                            <path d="M2 5h12"/>
                            <path d="M7 2h1"/>
                            <path d="m22 22-5-10-5 10"/>
                            <path d="M14 18h6"/>
                        </svg>
                    </button>
                    <button id="btn-open-accounts" class="icon-button qingtian-account-ui-hidden" type="button" title="账号管理" aria-hidden="true" tabindex="-1" style="display:none">👤</button>
                    <button id="btn-open-bridge" class="icon-button" type="button" title="远程桥接">🤖</button>
                    <button id="btn-open-web" class="icon-button" type="button" title="在浏览器打开，浏览器端未运行" style="display:none">💻</button>
                    <button id="btn-theme-toggle" class="icon-button" type="button" title="切换深色模式">☾</button>
                    <button id="btn-open-settings" class="icon-button" type="button" title="打开设置">⚙</button>
                </div>
            </div>

            <div class="metric-grid">
                <div class="metric-card">
                    <div class="metric-label">MCP 模式</div>
                    <div class="metric-inline">
                        <span id="port-dot" class="status-dot online"></span>
                        <span id="port-status-text" class="metric-status">Stdio</span>
                        <button id="btn-refresh-port" class="ghost-button" type="button" title="会改写 mcp.json 并可能让 Cursor 重启 MCP。正常使用请勿点。">刷新配置（慎用）</button>
                    </div>
                </div>
                <div class="metric-card">
                    <div class="metric-label">当前窗口</div>
                    <div class="metric-inline" style="gap:8px">
                        <span style="font-weight:600;font-size:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${workspaceName}">${workspaceName}</span>
                    </div>
                </div>
                <div class="metric-card">
                    <div class="metric-label">Subscription</div>
                    <div class="metric-inline" style="flex-direction:column;align-items:flex-start;gap:6px;">
                        <span id="license-plan-text" style="font-size:12px;font-weight:600;">Trial</span>
                        <span id="license-time-text" class="metric-countdown">--:--:--</span>
                        <button id="btn-show-renewal" class="soft-button" type="button" style="margin-top:2px;">Renew / Pay USDT</button>
                    </div>
                </div>
            </div>
            <div id="session-keepalive-banner" class="runtime-notice hidden" data-level="info" style="margin-top:10px;">
                <div class="runtime-notice-copy">
                    <div id="session-keepalive-title" class="runtime-notice-title">Session keepalive</div>
                    <div id="session-keepalive-message" class="runtime-notice-message">Idle</div>
                </div>
                <div class="runtime-notice-actions">
                    <button id="btn-resume-loop" class="soft-button" type="button">拉回循环</button>
                </div>
            </div>
        </section>

        <!-- 账单详情弹窗 -->
        <div id="billing-modal" class="billing-modal hidden">
            <div class="billing-modal-overlay"></div>
            <div class="billing-modal-content">
                <div class="billing-modal-header">
                    <h3 id="billing-modal-title">账单详情</h3>
                    <div class="billing-modal-header-actions">
                        <button id="btn-billing-modal-refresh" class="icon-button billing-modal-refresh-btn" type="button" title="刷新账单详情" aria-label="刷新账单详情" data-icon="refresh"></button>
                        <button id="btn-billing-modal-close" class="ghost-button" type="button" title="关闭">×</button>
                    </div>
                </div>
                <div id="billing-modal-body" class="billing-modal-body">
                    <div class="billing-modal-loading">加载中...</div>
                </div>
            </div>
        </div>

        <div id="update-notice-modal" class="update-notice-modal hidden" aria-hidden="true">
            <div id="update-notice-overlay" class="update-notice-overlay"></div>
            <div class="update-notice-dialog">
                <div class="update-notice-header">
                    <div>
                        <div class="update-notice-kicker">SLASHSUBS NOTICE</div>
                        <h3 id="update-notice-title">更新公告</h3>
                        <div id="update-notice-time" class="update-notice-time"></div>
                    </div>
                    <button id="btn-update-notice-close" class="icon-button" type="button" title="关闭">×</button>
                </div>
                <div id="update-notice-body" class="update-notice-body notice-rich-body"></div>
                <div class="update-notice-footer">
                    <label class="update-notice-suppress">
                        <input id="update-notice-suppress" type="checkbox" />
                        <span>下次启动时不再展示此公告</span>
                    </label>
                    <div class="update-notice-actions">
                        <button id="btn-update-notice-later" class="ghost-button" type="button">稍后再看</button>
                        <button id="btn-update-notice-action" class="primary-button" type="button">查看详情</button>
                    </div>
                </div>
            </div>
            <div id="update-notice-image-preview" class="update-notice-image-preview hidden" aria-hidden="true">
                <button id="btn-update-notice-image-close" class="update-notice-image-close" type="button" aria-label="关闭图片预览">×</button>
                <img id="update-notice-image" src="" alt="" />
            </div>
        </div>

        <div id="agent-team-modal" class="agent-team-modal hidden">
            <div id="agent-team-backdrop" class="agent-team-backdrop"></div>
            <div class="agent-team-dialog">
                <div class="agent-team-header">
                    <div>
                        <div class="agent-team-kicker">Agent Team</div>
                        <h3>群聊工作台</h3>
                    </div>
                    <div class="agent-team-header-actions">
                        <button id="btn-agent-team-refresh" class="ghost-button" type="button">刷新</button>
                        <button id="btn-agent-team-close" class="icon-button" type="button" title="关闭">×</button>
                    </div>
                </div>
                <div class="agent-team-body">
                    <aside class="agent-team-rail">
                        <div class="agent-team-section">
                            <div class="agent-team-section-title">群组</div>
                            <div id="agent-team-group-list" class="agent-team-group-list"></div>
                        </div>
                        <div class="agent-team-section">
                            <div class="agent-team-section-title">创建群组</div>
                            <input id="agent-team-group-name" class="agent-team-input" type="text" placeholder="群名称" />
                            <input id="agent-team-group-goal" class="agent-team-input" type="text" placeholder="目标，可选" />
                            <div id="agent-team-member-checks" class="agent-team-member-checks"></div>
                            <button id="btn-agent-team-create" class="primary-button agent-team-full" type="button">创建群组</button>
                        </div>
                    </aside>
                    <main class="agent-team-main">
                        <div id="agent-team-timeline" class="agent-team-timeline"></div>
                        <div class="agent-team-composer">
                            <textarea id="agent-team-input" class="agent-team-textarea" rows="3" placeholder="@CH-2 检查当前逻辑，或 @全体 同步目标"></textarea>
                            <div class="agent-team-composer-row">
                                <span>不写 @ 时默认发给当前群全体成员。</span>
                                <button id="btn-agent-team-send" class="primary-button" type="button">发送到群</button>
                            </div>
                        </div>
                    </main>
                    <aside class="agent-team-side">
                        <div class="agent-team-section">
                            <div class="agent-team-section-title">成员与角色</div>
                            <div id="agent-team-agent-list" class="agent-team-card-list"></div>
                        </div>
                        <div class="agent-team-section">
                            <div class="agent-team-section-title">任务</div>
                            <div id="agent-team-task-list" class="agent-team-card-list"></div>
                        </div>
                        <div class="agent-team-section">
                            <div class="agent-team-section-title">共享记忆</div>
                            <div id="agent-team-memory-list" class="agent-team-card-list"></div>
                        </div>
                    </aside>
                </div>
            </div>
        </div>

        <div id="collab-banner" class="collab-banner hidden">
            <div class="collab-banner-head">
                <div class="collab-banner-title">Agent Team 协同已开启</div>
                <button id="btn-open-agent-team" class="soft-button" type="button">打开群聊工作台</button>
            </div>
            <div class="collab-banner-steps">
                <div>1. 每个通道都是一个可协作 Agent，可拥有角色、任务和状态。</div>
                <div>2. Agent 之间通过 MCP 工具发送消息、共享上下文和记录决策。</div>
                <div>3. 任务通过 task_create / task_claim / task_update / task_close 流转</div>
            </div>
        </div>

        <section class="panel-card batch-retry-card" data-collapsed="true" hidden aria-hidden="true" style="display:none !important">
            <div class="batch-retry-header-wrap">
                <div id="batch-retry-header" class="batch-retry-header" role="button" tabindex="0" aria-expanded="false">
                    <span class="batch-retry-head-left">
                        <span class="batch-retry-icon">↻</span>
                        <span class="batch-retry-titles">
                            <span class="batch-retry-title-row">
                                <span class="batch-retry-title">批量会话重试</span>
                                <button id="btn-batch-retry-settings" class="batch-retry-inline-settings" type="button" title="批量会话重试设置" aria-label="批量会话重试设置" data-icon="settings"></button>
                                <button id="btn-batch-retry-history" class="batch-retry-inline-history" type="button" title="重试历史" aria-label="重试历史"></button>
                            </span>
                            <span id="batch-retry-status-line" class="batch-retry-subline">未运行 · 点击展开配置</span>
                        </span>
                    </span>
                    <span class="batch-retry-head-right">
                        <span id="batch-retry-running-pill" class="batch-retry-pill hidden">运行中</span>
                        <span id="batch-retry-toggle-arrow" class="batch-retry-arrow">▾</span>
                    </span>
                </div>
            </div>
            <div id="batch-retry-body" class="batch-retry-body hidden">
                <div class="batch-retry-console">
                    <div class="batch-retry-console-main">
                        <div class="batch-retry-console-kicker">任务控制台</div>
                        <div id="batch-retry-console-title" class="batch-retry-console-title">待命</div>
                        <div class="batch-retry-progress-bar"><div id="batch-retry-progress-fill" class="batch-retry-progress-fill"></div></div>
                        <div class="batch-retry-console-meta">
                            <span id="batch-retry-progress-count">0/0 完成</span>
                            <span id="batch-retry-console-mode">补丁已就绪</span>
                        </div>
                    </div>
                    <div class="batch-retry-console-percent">
                        <strong id="batch-retry-progress-percent">0%</strong>
                        <span>完成度</span>
                    </div>
                </div>
                <div class="batch-retry-stats" id="batch-retry-stats">
                    <span><b id="batch-retry-stat-total">0</b> 总任务</span>
                    <span><b id="batch-retry-stat-working">0</b> 执行中</span>
                    <span><b id="batch-retry-stat-waiting">0</b> 等待</span>
                    <span><b id="batch-retry-stat-done">0</b> 完成</span>
                </div>
                <div class="batch-retry-bindings">
                    <div class="batch-retry-binding-row">
                        <span id="batch-retry-binding-status" class="batch-retry-binding-status">通道绑定：未刷新</span>
                        <span id="batch-retry-binding-reuse-hint" class="batch-retry-binding-reuse-hint hidden" title="设置未勾选“复用已绑定通道”，启动会跳过这些绑定，改为新建窗口或等待空闲 MCP 通道。">未复用 · 将新建</span>
                        <select id="batch-retry-bind-channel">
                            ${Array.from({ length: Math.min(channelCount, 50) }, (_, i) => `<option value="${i + 1}">CH-${i + 1}</option>`).join('')}
                        </select>
                        <button id="btn-batch-retry-bind-active" class="soft-button" type="button">绑定当前 Cursor 会话</button>
                        <button id="btn-batch-retry-refresh-bindings" class="soft-button" type="button">刷新绑定</button>
                    </div>
                    <div id="batch-retry-binding-list" class="batch-retry-binding-list">先在 Cursor 中点选目标会话，再点“绑定当前 Cursor 会话”。</div>
                </div>
                <div class="batch-retry-phase hidden" id="batch-retry-progress-wrap">
                    <div class="batch-retry-phase-head">
                        <span>执行阶段</span>
                        <span id="batch-retry-progress-text">准备中…</span>
                    </div>
                    <div class="batch-retry-phase-steps">
                        <span class="batch-retry-phase-step" data-phase="queued"><span class="batch-retry-phase-dot"></span><span class="batch-retry-phase-label">准备</span></span>
                        <span class="batch-retry-phase-step" data-phase="silent_start"><span class="batch-retry-phase-dot"></span><span class="batch-retry-phase-label">静默重试</span></span>
                        <span class="batch-retry-phase-step" data-phase="retrying"><span class="batch-retry-phase-dot"></span><span class="batch-retry-phase-label">执行重试</span></span>
                        <span class="batch-retry-phase-step" data-phase="confirm_success"><span class="batch-retry-phase-dot"></span><span class="batch-retry-phase-label">证据确认</span></span>
                        <span class="batch-retry-phase-step" data-phase="done"><span class="batch-retry-phase-dot"></span><span class="batch-retry-phase-label">完成</span></span>
                    </div>
                </div>
                <div class="batch-retry-actions">
                    <label class="batch-retry-session-inline">
                        <span>会话数量</span>
                        <input id="batch-retry-session-count" type="number" min="1" max="50" value="5" />
                    </label>
                    <label class="batch-retry-network-inline">
                        <span>网络切换</span>
                        <select id="batch-retry-network-type" title="Cursor HTTP Compatibility Mode">
                            <option value="unknown">未识别</option>
                            <option value="1.0">1.0</option>
                            <option value="1.1">1.1</option>
                            <option value="2.0">2.0</option>
                        </select>
                    </label>
                    <button id="btn-batch-retry-start" class="primary-button" type="button">启动</button>
                    <button id="btn-batch-retry-stop" class="ghost-button" type="button" disabled>全部停止</button>
                </div>
                <div id="batch-retry-task-list" class="batch-retry-tasks">
                    <div class="batch-retry-empty">暂未启动 · 点击“启动”开始批量会话重试</div>
                </div>
            </div>
        </section>

        <div id="batch-retry-channel-wait-toast" class="batch-retry-channel-wait-toast hidden" aria-live="polite">
            <span id="batch-retry-channel-wait-icon" class="batch-retry-channel-wait-icon loading"></span>
            <span class="batch-retry-channel-wait-copy">
                <strong id="batch-retry-channel-wait-title">等待 MCP 通道</strong>
                <span id="batch-retry-channel-wait-text">请打开新增通道的 MCP 开关，插件会自动继续。</span>
            </span>
            <button id="btn-batch-retry-channel-wait-close" class="batch-retry-channel-wait-close" type="button" aria-label="关闭等待提示">×</button>
        </div>

        <div id="batch-retry-settings-modal" class="batch-retry-settings-modal hidden" aria-hidden="true">
            <div id="batch-retry-settings-overlay" class="batch-retry-modal-overlay"></div>
            <div class="batch-retry-settings-dialog" role="dialog" aria-modal="true" aria-labelledby="batch-retry-settings-title">
                <div class="batch-retry-settings-head">
                    <div>
                        <div id="batch-retry-settings-title" class="batch-retry-settings-title">批量会话重试设置</div>
                        <div class="batch-retry-settings-subtitle">仅保留模式切换：静默重试会并发，可见重试会串行。</div>
                    </div>
                    <button id="btn-batch-retry-settings-close" class="batch-retry-modal-close" type="button" aria-label="关闭设置">×</button>
                </div>
                <div class="batch-retry-settings-list">
                    <div class="batch-retry-setting-row">
                        <label class="batch-retry-check"><span>重试间隔</span></label>
                        <span class="batch-retry-setting-desc">单位毫秒，默认 300ms；建议范围 100-30000。</span>
                        <input id="batch-retry-interval-setting" class="batch-retry-number-input" type="number" min="100" max="30000" step="50" value="300" />
                    </div>
                    <div class="batch-retry-setting-row">
                        <label class="batch-retry-check"><input id="batch-retry-use-bridge" type="checkbox" checked /> 静默重试</label>
                        <span class="batch-retry-setting-desc">勾选后静默并发；关闭后使用可见串行，不再显示独立并发开关。</span>
                        <button class="batch-retry-help-btn" type="button" data-help="useBridge" aria-label="查看静默重试说明">?</button>
                    </div>
                    <div class="batch-retry-setting-row">
                        <label class="batch-retry-check"><input id="batch-retry-reuse" type="checkbox" /> 复用已绑定通道</label>
                        <span class="batch-retry-setting-desc">勾选后优先使用上方面板绑定的 CH 会话；缺失的 CH 再新建补齐。</span>
                        <button class="batch-retry-help-btn" type="button" data-help="reuseExisting" aria-label="查看复用绑定说明">?</button>
                    </div>
                    <div class="batch-retry-setting-row">
                        <label class="batch-retry-check"><input id="batch-retry-custom-join" type="checkbox" /> 自定义重试发送语</label>
                        <span class="batch-retry-setting-desc">仅作用于批量重试；勾选后用下方模板替换发送语，{X} 自动换成通道号。</span>
                        <button class="batch-retry-help-btn" type="button" data-help="customJoinPhrase" aria-label="查看自定义发送语说明">?</button>
                    </div>
                    <div class="batch-retry-setting-row batch-retry-join-template-row hidden" id="batch-retry-join-template-row">
                        <textarea id="batch-retry-join-template" class="batch-retry-join-template" rows="3" placeholder="例如：请根据KCMCP-{X}的方式去执行"></textarea>
                    </div>
                </div>
            </div>
        </div>
        <div id="batch-retry-help-modal" class="batch-retry-help-modal hidden" aria-hidden="true">
            <div id="batch-retry-help-overlay" class="batch-retry-modal-overlay"></div>
            <div class="batch-retry-help-dialog" role="dialog" aria-modal="true" aria-labelledby="batch-retry-help-title">
                <div class="batch-retry-settings-head">
                    <div id="batch-retry-help-title" class="batch-retry-settings-title">设置说明</div>
                    <button id="btn-batch-retry-help-close" class="batch-retry-modal-close" type="button" aria-label="关闭说明">×</button>
                </div>
                <div id="batch-retry-help-body" class="batch-retry-help-body"></div>
            </div>
        </div>
        <div id="batch-retry-history-modal" class="batch-retry-history-modal hidden" aria-hidden="true">
            <div id="batch-retry-history-overlay" class="batch-retry-modal-overlay"></div>
            <div class="batch-retry-history-dialog" role="dialog" aria-modal="true" aria-labelledby="batch-retry-history-title">
                <div class="batch-retry-settings-head">
                    <div>
                        <div id="batch-retry-history-title" class="batch-retry-settings-title">重试历史</div>
                        <div class="batch-retry-settings-subtitle">记录每个会话任务的执行时间、重试次数、结果和窗口 id。</div>
                    </div>
                    <button id="btn-batch-retry-history-close" class="batch-retry-modal-close" type="button" aria-label="关闭历史">×</button>
                </div>
                <div class="batch-retry-history-summary">
                    <span>成功平均耗时 <b id="batch-retry-history-avg-time">--</b></span>
                    <span>成功平均次数 <b id="batch-retry-history-avg-retries">--</b></span>
                </div>
                <div class="batch-retry-history-toolbar">
                    <label>
                        <span>排序</span>
                        <select id="batch-retry-history-sort">
                            <option value="latest">最近执行优先</option>
                            <option value="time_desc">耗时从长到短</option>
                            <option value="time_asc">耗时从短到长</option>
                            <option value="retries_desc">次数从多到少</option>
                            <option value="retries_asc">次数从少到多</option>
                        </select>
                    </label>
                </div>
                <div id="batch-retry-history-list" class="batch-retry-history-list">
                    <div class="batch-retry-history-empty">暂无重试历史</div>
                </div>
            </div>
        </div>

        <section class="panel-card channel-card">
            <div class="panel-header">
                <div>
                    <h2 class="panel-title">通道选择</h2>
                    <p class="panel-subtitle">每个 Cursor 对话绑定一个通道，多会话并行互不干扰。</p>
                </div>
                <span id="active-channel-hint" class="channel-hint">当前：CH-1</span>
            </div>
            <div id="channel-rail" class="channel-rail" data-count="${channelCount}" data-max="">
            </div>
        </section>

        <section class="panel-card composer-card">
            <div class="panel-header">
                <div>
                    <h2 class="panel-title">指令中心</h2>
                    <p class="panel-subtitle">输入消息后直接发送到对应通道的消息队列。</p>
                </div>
                <div id="ai-presence" class="ai-presence ai-presence-unknown" title="AI 状态 · 悬停查看说明">
                    <span class="ai-presence-dot"></span>
                    <span id="ai-presence-text">检测中…</span>
                </div>
            </div>

            <div id="ai-call-card" class="ai-call-card hidden">
                <button id="ai-return-header" class="ai-return-header" type="button">
                        <span class="pulse-dot"></span>
                        <span class="ai-return-meta">
                            <span id="ai-header-title" class="ai-return-title">等待你的输入...</span>
                            <span id="ai-header-subtitle" class="ai-return-subtitle">完整回复已归档，点击展开查看</span>
                        </span>
                    <span id="ai-reply-clear" class="ai-reply-clear" title="清除本条 AI 回复">×</span>
                    <span id="ai-toggle-arrow" class="ai-toggle-arrow">▾</span>
                </button>
                <div id="ai-return-box" class="ai-return-box hidden"></div>
                <div id="choices-list" class="choices-list"></div>
            </div>

            <div class="input-editor-wrap" id="drop-zone">
                <textarea id="user-input" placeholder="输入指令，@引用文件，拖拽/粘贴图片 | Enter发送，Shift+Enter换行"></textarea>
                <div id="file-search-dropdown" class="file-search-dropdown hidden"></div>
            </div>

            <div class="composer-toolbar">
                <div class="toolbar-menu">
                    <button class="soft-button toolbar-menu-trigger" type="button" data-toolbar-menu="attachments" aria-expanded="false">
                        <span>附件</span><span class="toolbar-menu-caret">▾</span>
                    </button>
                    <div class="toolbar-menu-panel hidden" data-toolbar-menu-panel="attachments">
                        <button id="btn-select-file" class="toolbar-menu-item" type="button">文件</button>
                        <button id="btn-select-folder" class="toolbar-menu-item" type="button">文件夹</button>
                    </div>
                </div>
                <div class="toolbar-menu">
                    <button class="soft-button toolbar-menu-trigger" type="button" data-toolbar-menu="start" aria-expanded="false">
                        <span>开场</span><span class="toolbar-menu-caret">▾</span>
                    </button>
                    <div class="toolbar-menu-panel hidden" data-toolbar-menu-panel="start">
                        <button id="btn-copy-prompt" class="toolbar-menu-item" type="button">复制开场</button>
                        <button id="btn-send-start-prompt" class="toolbar-menu-item" type="button" title="自动打开 Cursor 对话并发送当前通道开场白；失败时会保留剪贴板兜底">一键开场</button>
                        <button id="btn-resume-loop-menu" class="toolbar-menu-item" type="button" title="在原会话拉回 check_messages，不新开场、不额外耗额度">拉回循环（不新开场）</button>
                        <button id="btn-custom-start-prompt" class="toolbar-menu-item" type="button">自定义开场语</button>
                    </div>
                </div>
                <div class="toolbar-menu toolbar-menu-right">
                    <button class="soft-button toolbar-menu-trigger" type="button" data-toolbar-menu="more" aria-expanded="false">
                        <span>更多</span><span class="toolbar-menu-caret">▸</span>
                    </button>
                    <div class="toolbar-menu-panel hidden" data-toolbar-menu-panel="more">
                        <button id="btn-copy-recovery" class="toolbar-menu-item" type="button" title="把当前通道的恢复上下文直接投递到队列，当前或下一个绑定该通道的窗口会自动接手">恢复上下文</button>
                        <button id="btn-mcp-picker" class="toolbar-menu-item" type="button" title="选择本轮希望 Agent 调用的 MCP">MCP</button>
                    </div>
                </div>
                <button id="btn-stop-turn" class="danger-button" type="button" title="停止当前通道正在执行/等待的任务，不新开场、不耗新额度">停止当前</button>
                <button id="btn-send" class="primary-button" type="button">发送消息</button>
            </div>
            <div class="quick-commands-wrap">
                <div id="quick-commands" class="quick-commands-row">
                    <div class="quick-cmd-empty">加载快捷指令...</div>
                </div>
                <div class="quick-commands-tools">
                    <button id="btn-quick-add" class="quick-cmd-add" type="button" title="新增快捷指令">＋</button>
                    <button id="btn-quick-reset" class="quick-cmd-reset" type="button" title="恢复默认">↻</button>
                </div>
            </div>
            <div id="quick-commands-form" class="quick-commands-form hidden">
                <input id="quick-cmd-label" type="text" placeholder="按钮文字，例如：写 README" maxlength="40" />
                <textarea id="quick-cmd-text" rows="2" placeholder="点击后填入输入框的内容..." maxlength="2000"></textarea>
                <div class="quick-commands-form-actions">
                    <button id="btn-quick-save" class="soft-button" type="button">保存</button>
                    <button id="btn-quick-cancel" class="ghost-button" type="button">取消</button>
                </div>
            </div>

            <div class="attachment-panel">
                <div class="attachment-header">
                    <span class="attachment-title">当前附件</span>
                    <span class="attachment-hint">支持图片粘贴与工作区文件</span>
                </div>
                <div id="file-list" class="file-list">
                    <div class="file-empty">暂无文件</div>
                </div>
            </div>
        </section>

        <section class="panel-card history-card">
            <div class="panel-header">
                <div>
                    <h2 class="panel-title">提交指令记录</h2>
                    <p class="panel-subtitle">最近 50 次交互自动保存，可导出为 Markdown。</p>
                </div>
                <div class="history-tools">
                    <span id="history-count" class="count-pill">0</span>
                    <button id="btn-clear-history" class="export-button" type="button" style="color:var(--danger)">清空</button>
                    <button id="btn-export-history" class="export-button" type="button">导出</button>
                </div>
            </div>
            <div id="history-list" class="history-list">
                <div class="history-empty">暂无记录</div>
            </div>
        </section>

        <div id="recovery-transfer-modal" class="billing-modal hidden">
            <div id="recovery-transfer-overlay" class="billing-modal-overlay"></div>
            <div class="billing-modal-content recovery-transfer-content">
                <div class="billing-modal-header recovery-transfer-header">
                    <div>
                        <h3>恢复上下文</h3>
                        <div class="recovery-transfer-subtitle">当前按钮所属通道作为恢复来源；复杂操作收敛到这里，不占主界面空间。</div>
                    </div>
                    <div class="recovery-transfer-header-actions">
                        <button id="btn-recovery-transfer-refresh" class="ghost-button" type="button" title="重新检测在线/待命通道">刷新</button>
                        <button id="btn-recovery-transfer-close" class="ghost-button" type="button" title="关闭">×</button>
                    </div>
                </div>
                <div class="billing-modal-body recovery-transfer-body">
                    <div class="recovery-transfer-section">
                        <div class="recovery-transfer-label">恢复来源</div>
                        <div id="recovery-transfer-source" class="recovery-transfer-source">CH-1</div>
                    </div>
                    <div class="recovery-transfer-section">
                        <div class="recovery-transfer-label">恢复方式</div>
                        <label class="recovery-transfer-option" data-mode="current">
                            <input type="radio" name="recovery-transfer-mode" value="current" checked />
                            <span class="recovery-transfer-option-copy">
                                <strong>当前通道恢复</strong>
                                <span>把该通道已归档上下文重新投递给当前通道，适合当前窗口还在但需要补恢复。</span>
                            </span>
                        </label>
                        <label class="recovery-transfer-option" data-mode="transfer">
                            <input type="radio" name="recovery-transfer-mode" value="transfer" />
                            <span class="recovery-transfer-option-copy">
                                <strong>转移到其他通道</strong>
                                <span>当原通道挂掉时，把当前通道的上下文显式交给另一个通道接手。</span>
                            </span>
                        </label>
                    </div>
                    <div id="recovery-transfer-target-section" class="recovery-transfer-section hidden">
                        <div class="recovery-transfer-label">目标通道</div>
                        <div id="recovery-transfer-target-list" class="recovery-transfer-target-list"></div>
                    </div>
                </div>
                <div class="recovery-transfer-footer">
                    <button id="btn-recovery-transfer-cancel" class="ghost-button" type="button">取消</button>
                    <button id="btn-recovery-transfer-confirm" class="primary-button" type="button">开始恢复</button>
                </div>
            </div>
        </div>

        <div id="settings-backdrop" class="settings-backdrop hidden"></div>
        <div id="start-prompt-modal" class="start-prompt-modal hidden" aria-hidden="true">
            <div id="start-prompt-modal-overlay" class="start-prompt-modal-overlay"></div>
            <div class="start-prompt-dialog" role="dialog" aria-modal="true" aria-labelledby="start-prompt-modal-title">
                <div class="start-prompt-head">
                    <div>
                        <div class="start-prompt-kicker">开场语</div>
                        <div id="start-prompt-modal-title" class="start-prompt-title">自定义开场语</div>
                    </div>
                    <button id="btn-start-prompt-close" class="start-prompt-close" type="button" aria-label="关闭">×</button>
                </div>
                <div class="start-prompt-body">
                    <div class="start-prompt-desc">使用 <code>{X}</code> 代表当前通道号，例如 <code>qtwx-mcp-{X}</code>。保存后，复制开场和一键开场都会使用这段内容。</div>
                    <textarea id="start-prompt-template" class="start-prompt-template" rows="7" placeholder="留空则使用默认开场语。"></textarea>
                    <div class="start-prompt-preview">
                        <div class="start-prompt-preview-label">当前通道预览</div>
                        <div id="start-prompt-preview-text" class="start-prompt-preview-text"></div>
                    </div>
                </div>
                <div class="start-prompt-actions">
                    <button id="btn-start-prompt-reset" class="ghost-button" type="button">恢复默认</button>
                    <span class="start-prompt-actions-spacer"></span>
                    <button id="btn-start-prompt-cancel" class="ghost-button" type="button">取消</button>
                    <button id="btn-start-prompt-save" class="primary-button" type="button">保存</button>
                </div>
            </div>
        </div>
        <aside id="settings-drawer" class="settings-drawer">
            <div class="settings-head">
                <div>
                    <div id="settings-kicker" class="settings-kicker">偏好设置</div>
                    <div id="settings-title" class="settings-title">显示与交互</div>
                </div>
                <button id="btn-close-settings" class="icon-button" type="button">×</button>
            </div>
            <div class="settings-body">
                <div id="settings-preferences-section">
                    <div class="setting-row">
                        <div class="setting-info">
                            <div class="setting-label">Subscription</div>
                            <div class="setting-desc" id="setting-subscription-desc">2-day free trial, then Monthly $9.9 or Yearly $49 (USDT-TRC20).</div>
                        </div>
                        <div class="setting-control" style="display:flex;gap:6px;flex-wrap:wrap;">
                            <button id="btn-settings-renew" class="soft-button" type="button">Renew / Pay</button>
                            <button id="btn-settings-force-expire" class="danger-button" type="button" title="QA: force expire now">Test expire</button>
                            <button id="btn-settings-debug-unlock" class="soft-button" type="button" title="QA: simulate paid unlock">Simulate paid</button>
                        </div>
                    </div>
                    <div class="setting-row">
                        <div class="setting-info">
                            <div class="setting-label">Send shortcut</div>
                            <div class="setting-desc">选择提交消息的快捷键。</div>
                        </div>
                        <div class="setting-control">
                            <select id="setting-send-mode">
                                <option value="enter">Enter发送</option>
                                <option value="ctrl-enter">Ctrl+Enter发送</option>
                            </select>
                        </div>
                    </div>
                    <div class="setting-row">
                        <div class="setting-info">
                            <div class="setting-label">保活机制</div>
                            <div class="setting-desc">AI 空闲时持续轮询等待消息。关闭后 AI 超时将自然结束对话。</div>
                        </div>
                        <div class="setting-control">
                            <label class="setting-inline">
                                <input type="checkbox" id="setting-keepalive-enabled" checked />
                                <span>启用</span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-row" id="setting-keepalive-minutes-row">
                        <div class="setting-info">
                            <div class="setting-label">保活间隔</div>
                            <div class="setting-desc">每隔多少分钟发送一次保活信号（1~120）。</div>
                        </div>
                        <div class="setting-control">
                            <input type="number" id="setting-keepalive-minutes" min="1" max="120" value="45" style="width:64px" />
                            <span style="margin-left:4px;opacity:0.7">分钟</span>
                        </div>
                    </div>
                    <div class="setting-row">
                        <div class="setting-info">
                            <div class="setting-label">回复通知</div>
                            <div class="setting-desc">AI 回复时弹出系统通知，Cursor 最小化时也能收到提醒。</div>
                        </div>
                        <div class="setting-control">
                            <label class="setting-inline">
                                <input type="checkbox" id="setting-notify-on-reply" />
                                <span>启用</span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-row">
                        <div class="setting-info">
                            <div class="setting-label">浏览器端</div>
                            <div class="setting-desc">在浏览器打开独立面板，数据与插件共享。仅本机访问。</div>
                        </div>
                        <div class="setting-control">
                            <label class="setting-inline">
                                <input type="checkbox" id="setting-web-server-enabled" />
                                <span>启用</span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-row" id="setting-web-server-url-row">
                        <div class="setting-info">
                            <div class="setting-label">访问地址</div>
                            <div class="setting-desc" id="setting-web-server-url-desc">未启用</div>
                        </div>
                        <div class="setting-control">
                            <button id="btn-open-web-browser" class="soft-button" type="button">在浏览器打开</button>
                        </div>
                    </div>
                    <div class="setting-row">
                        <div class="setting-info">
                            <div class="setting-label">Agent Team 协同</div>
                            <div class="setting-desc">启用角色、任务、记忆、事件流和上下文共享工具。</div>
                        </div>
                        <div class="setting-control">
                            <label class="setting-inline">
                                <input type="checkbox" id="setting-collab-enabled" />
                                <span>开启</span>
                            </label>
                        </div>
                    </div>
                    <div class="setting-row">
                        <div class="setting-info">
                            <div class="setting-label">协作方式</div>
                            <div class="setting-desc" id="setting-collab-hint">Agent 通过 MCP 工具创建任务、互发消息、共享上下文和记录决策。</div>
                        </div>
                    </div>
                    <div class="setting-row setting-row-danger">
                        <div class="setting-info">
                            <div class="setting-label">退出激活</div>
                            <div class="setting-desc">清除本机授权状态；删除后需重新试用或付款开通。</div>
                        </div>
                        <div class="setting-control">
                            <button type="button" id="btn-logout" class="danger-button">退出</button>
                        </div>
                    </div>
                    <div class="setting-row setting-row-danger">
                        <div class="setting-info">
                            <div class="setting-label">完全清理插件数据</div>
                            <div class="setting-desc">清除消息队列、运行时状态、mcp.json 条目、规则文件、settings 配置和插件全局状态。</div>
                        </div>
                        <div class="setting-control">
                            <button type="button" id="btn-full-cleanup" class="danger-button">完全清理...</button>
                        </div>
                    </div>
                </div>
            </div>
        </aside>

        <aside id="bridge-panel" class="setting-panel" style="display:none">
            <div class="setting-panel-header">
                <h2>远程桥接</h2>
                <button id="btn-close-bridge" class="icon-button" type="button" title="关闭">×</button>
            </div>
            <div class="setting-panel-body">
                <p class="setting-panel-desc">通过 Telegram 远程操控 AI Agent。在手机上发消息，AI 在 Cursor 中处理后回复到手机。</p>
                <div class="setting-row">
                    <div class="setting-info">
                        <div class="setting-label">启用桥接</div>
                        <div class="setting-desc">开启后 AI 回复会同步推送到 Telegram。</div>
                    </div>
                    <div class="setting-control">
                        <label class="setting-inline">
                            <input type="checkbox" id="setting-bridge-enabled" />
                            <span>启用</span>
                        </label>
                    </div>
                </div>
                <div class="setting-row" id="setting-bridge-proxy-row">
                    <div class="setting-info">
                        <div class="setting-label">代理配置</div>
                        <div class="setting-desc">开启后读取 Cursor/http、环境变量或系统代理；关闭后强制直连，不继承任何代理。</div>
                    </div>
                    <div class="setting-control">
                        <label class="setting-inline">
                            <input type="checkbox" id="setting-bridge-use-proxy" checked />
                            <span>使用代理</span>
                        </label>
                    </div>
                </div>
                <div id="setting-bridge-token-row" style="display:none">
                    <div class="setting-row" style="flex-direction:column;align-items:stretch">
                        <div class="setting-info" style="margin-bottom:6px">
                            <div class="setting-label">Bot Token</div>
                            <div class="setting-desc">在 Telegram 找 @BotFather，使用 /newbot 创建机器人，复制 Token 粘贴到这里。</div>
                        </div>
                        <div style="display:flex;align-items:center;gap:6px;position:relative">
                            <input type="password" id="setting-bridge-bot-token" placeholder="粘贴 Telegram Bot Token" style="flex:1;font-family:monospace;padding-right:32px" />
                            <button type="button" id="btn-toggle-token-visibility" class="token-eye-btn" title="显示/隐藏">显隐</button>
                        </div>
                    </div>
                </div>
                <div class="setting-row" id="setting-bridge-channel-row" style="display:none">
                    <div class="setting-info">
                        <div class="setting-label">桥接通道</div>
                        <div class="setting-desc">远程消息绑定到哪个 MCP 通道。</div>
                    </div>
                    <div class="setting-control">
                        <select id="setting-bridge-channel" style="width:80px"></select>
                    </div>
                </div>
                <div id="setting-bridge-status-row" style="display:none;margin-top:10px">
                    <div class="bridge-auto-hint">
                        <span class="bridge-auto-dot"></span>
                        <span>桥接服务已自动启动，日志见“输出”里的“SlashSubs Bridge”。</span>
                    </div>
                </div>
            </div>
        </aside>

        <aside id="mcp-picker-panel" class="setting-panel" style="display:none">
            <div class="setting-panel-header">
                <h2>MCP 快捷调用</h2>
                <button id="btn-close-mcp-picker" class="icon-button" type="button" title="关闭">×</button>
            </div>
            <div class="setting-panel-body">
                <div class="setting-panel-desc">勾选后，下一次发送消息时会自动附加 MCP 调用要求。</div>
                <label class="mcp-picker-select-all">
                    <input type="checkbox" id="mcp-picker-select-all" />
                    <span>全选</span>
                </label>
                <div id="mcp-picker-list" class="mcp-picker-list">
                    <div class="mcp-picker-empty">正在读取当前工作区 MCP...</div>
                </div>
            </div>
        </aside>

        <footer class="support-footer">
            <div><a href="https://chat.whatsapp.com/EJUfSlyZxlQ0bYCtdYqsQA" id="footer-whatsapp-link" target="_blank" rel="noopener noreferrer">WhatsApp Group</a></div>
        </footer>
    </div>
    <script>
    (function(){
      // acquireVsCodeApi 只能调用一次；缓存后覆盖全局，避免后续 webview.js 再调时报错导致整页菜单无响应
      const vscodeApi = (function(){
        try {
          if (window.__qtVscodeApi) return window.__qtVscodeApi;
          const api = acquireVsCodeApi();
          window.__qtVscodeApi = api;
          window.acquireVsCodeApi = function(){ return api; };
          return api;
        } catch (e) {
          return window.__qtVscodeApi || { postMessage: function(){}, getState: function(){ return {}; }, setState: function(){} };
        }
      })();
      function $(id){ return document.getElementById(id); }
      function setFeedback(text, ok){
        const el = $('license-feedback');
        if(!el) return;
        el.textContent = text || '';
        el.style.color = ok ? '#3dd68c' : '#ff7b72';
      }
      window.__qtRenewalPreview = false;
      function renderPayment(msg){
        const gate = $('license-gate');
        const app = $('app');
        if (msg && msg.preview === true) window.__qtRenewalPreview = true;
        if (msg && msg.exitPreview === true) window.__qtRenewalPreview = false;
        // Keep paywall visible while previewing Renew / Pay, even if status polls say activated
        const activated = window.__qtRenewalPreview ? false : !!(msg && msg.activated);
        if(gate) gate.classList.toggle('hidden', activated);
        if(app) app.classList.toggle('hidden', !activated);
        try { if (!activated && gate) gate.scrollIntoView({ behavior: 'smooth', block: 'start' }); } catch (e) {}
        const payment = (msg && msg.payment) || {};
        const qr = $('license-qr');
        const meta = $('license-pay-meta');
        const desc = $('license-desc');
        const trialBtn = $('btn-start-trial');
        if(desc){
          desc.textContent = activated
            ? 'Licensed'
            : (payment.configured
              ? 'Trial/subscription expired. Choose a plan, pay USDT (TRC20), then Verify payment.'
              : 'Trial/subscription expired. Payment address not configured.');
        }
        if(qr){
          if(payment.qrUrl){
            qr.src = payment.qrUrl;
            qr.style.display = 'block';
          } else {
            qr.removeAttribute('src');
            qr.style.display = 'none';
          }
        }
        if(meta){
          meta.innerHTML = [
            payment.selectedPlanLabel ? ('Plan: <b>' + payment.selectedPlanLabel + '</b> ($' + payment.amount + ' / ' + payment.selectedPlanDays + ' days)') : '',
            payment.network ? ('Network: <b>' + payment.network + '</b>') : '',
            payment.amount ? ('Pay exactly: <b>' + payment.amount + ' USDT</b>') : '',
            payment.address ? ('Address:<br/><code style="user-select:all;word-break:break-all;font-size:12px;">' + payment.address + '</code>') : 'Payment address missing',
            payment.note || ''
          ].filter(Boolean).join('<br/>');
        }
        var mBtn = $('btn-plan-monthly');
        var yBtn = $('btn-plan-yearly');
        if(mBtn && yBtn){
          var sid = payment.selectedPlanId || 'monthly';
          mBtn.style.opacity = sid === 'monthly' ? '1' : '.55';
          yBtn.style.opacity = sid === 'yearly' ? '1' : '.55';
          mBtn.style.outline = sid === 'monthly' ? '2px solid #3dd68c' : 'none';
          yBtn.style.outline = sid === 'yearly' ? '2px solid #3dd68c' : 'none';
        }
        if(trialBtn){
          // 本机试用名额用过后不再显示「Start free trial」
          const consumed = !!(msg && (msg.trialConsumed || (msg.license && msg.license.trialConsumed)));
          const showTrial = !activated && !consumed && !(msg && msg.license && msg.license.expired);
          trialBtn.style.display = showTrial ? 'inline-block' : 'none';
        }
      }
      window.addEventListener('message', function(event){
        const msg = event.data || {};
        if(msg.command === 'licenseStatus'){
          renderPayment(msg);
          if(msg.activated){ stopPayPoll(); } else { startPayPoll(); }
        }
        if(msg.command === 'licenseResult'){
          setFeedback(msg.message || '', !!msg.success);
          if (msg.success && !msg.pending) {
            // Real unlock / password renew — leave preview
            window.__qtRenewalPreview = false;
          }
        }
        if(msg.command === 'status' && msg.data && msg.data.license){
          renderPayment({
            activated: !!(msg.data.license.activated),
            payment: msg.data.license.payment,
            license: msg.data.license,
            // do not pass preview/exitPreview; sticky flag decides
          });
        }
      });
      let payPollTimer = null;
      function startPayPoll(){
        if(payPollTimer) return;
        payPollTimer = setInterval(function(){
          const gate = $('license-gate');
          if(gate && !gate.classList.contains('hidden')){
            vscodeApi.postMessage({ command: 'verifyCryptoPayment' });
          }
        }, 15000);
      }
      function stopPayPoll(){
        if(payPollTimer){ clearInterval(payPollTimer); payPollTimer = null; }
      }
      document.addEventListener('DOMContentLoaded', function(){
        const input = $('license-code-input');
        const btn = $('btn-activate');
        const verifyBtn = $('btn-verify-payment');
        const debugUnlockBtn = $('btn-debug-unlock');
        const copyBtn = $('btn-copy-pay-address');
        const trialBtn = $('btn-start-trial');
        if(btn){
          btn.addEventListener('click', function(){
            const code = (input && input.value || '').trim();
            vscodeApi.postMessage({ command: 'activateLicense', code: code });
          });
        }
        if(verifyBtn){
          verifyBtn.addEventListener('click', function(){
            setFeedback('正在链上查询付款...', true);
            vscodeApi.postMessage({ command: 'verifyCryptoPayment' });
          });
        }
        if(debugUnlockBtn){
          debugUnlockBtn.addEventListener('click', function(){
            setFeedback('Simulating paid unlock...', true);
            vscodeApi.postMessage({ command: 'debugUnlockSelectedPlan' });
          });
        }
        if(input){
          input.addEventListener('keydown', function(e){
            if(e.key === 'Enter'){
              btn && btn.click();
            }
          });
        }
        if(copyBtn){
          copyBtn.addEventListener('click', function(){
            vscodeApi.postMessage({ command: 'copyPaymentAddress' });
          });
        }
        if(trialBtn){
          trialBtn.addEventListener('click', function(){
            vscodeApi.postMessage({ command: 'startLocalTrial' });
          });
        }
        // === Crypto method picker logic ===
        var cryptoGrid = $('crypto-method-grid');
        var cryptoDetails = $('crypto-pay-details');
        var cryptoBackBtn = $('btn-crypto-back');
        var currentMethodId = null;

        if (cryptoGrid) {
          cryptoGrid.addEventListener('click', function(ev){
            var card = ev.target.closest('.crypto-method-card');
            if (!card) return;
            ev.preventDefault();
            var methodId = card.getAttribute('data-method');
            if (!methodId) return;
            currentMethodId = methodId;
            setFeedback('', true);
            vscodeApi.postMessage({ command: 'selectCryptoMethod', methodId: methodId });
          });
        }
        if (cryptoBackBtn) {
          cryptoBackBtn.addEventListener('click', function(){
            if (cryptoDetails) cryptoDetails.style.display = 'none';
            if (cryptoGrid) cryptoGrid.style.display = '';
            currentMethodId = null;
          });
        }
        var copyPayAddr = $('btn-copy-pay-address');
        if (copyPayAddr) {
          copyPayAddr.addEventListener('click', function(){
            if (currentMethodId) {
              vscodeApi.postMessage({ command: 'copyCryptoAddress', methodId: currentMethodId });
            } else {
              vscodeApi.postMessage({ command: 'copyPaymentAddress' });
            }
          });
        }

        vscodeApi.postMessage({ command: 'getPaymentInfo' });
        vscodeApi.postMessage({ command: 'getCryptoMethods' });
        vscodeApi.postMessage({ command: 'getStatus' });
        startPayPoll();
      });

      // Handle crypto method responses
      window.addEventListener('message', function(event){
        var msg = event.data || {};
        if (msg.command === 'cryptoMethods') {
          var grid = document.getElementById('crypto-method-grid');
          if (!grid || !Array.isArray(msg.methods)) return;
          grid.innerHTML = '';
          msg.methods.forEach(function(m){
            if (!m.configured) return;
            var btn = document.createElement('button');
            btn.type = 'button';
            btn.className = 'crypto-method-card';
            btn.setAttribute('data-method', m.id);
            btn.innerHTML = '<span class="crypto-method-icon" style="color:' + m.color + ';">' + m.icon + '</span>' +
                            '<span class="crypto-method-label">' + m.label + '</span>';
            grid.appendChild(btn);
          });
          if (grid.children.length === 0) {
            grid.innerHTML = '<div style="grid-column:1/-1;text-align:center;color:#888;font-size:12px;padding:10px;">No payment methods configured. Set addresses in Settings → qingtian.cryptoAddresses</div>';
          }
        }
        if (msg.command === 'cryptoMethodDetails') {
          var details = document.getElementById('crypto-pay-details');
          var grid2 = document.getElementById('crypto-method-grid');
          if (!details) return;
          if (!msg.configured) {
            var fb = document.getElementById('license-feedback');
            if (fb) { fb.textContent = msg.message || 'Not configured'; fb.style.color = '#ff7b72'; }
            return;
          }
          if (grid2) grid2.style.display = 'none';
          details.style.display = '';
          var nameEl = document.getElementById('crypto-pay-method-name');
          var amtEl = document.getElementById('crypto-pay-amount');
          var netEl = document.getElementById('crypto-pay-network-badge');
          var addrEl = document.getElementById('crypto-pay-address');
          var warnEl = document.getElementById('crypto-pay-warning');
          var qrEl = document.getElementById('crypto-pay-qr');
          if (nameEl) nameEl.textContent = msg.methodLabel || '';
          if (amtEl) amtEl.textContent = '$' + (msg.amount || '0') + ' ' + (msg.currency || '');
          if (netEl) netEl.textContent = msg.network || '';
          if (addrEl) addrEl.textContent = msg.address || '';
          if (warnEl) warnEl.textContent = msg.note || '';
          if (qrEl && msg.qrUrl) { qrEl.src = msg.qrUrl; qrEl.style.display = ''; }
          else if (qrEl) { qrEl.style.display = 'none'; }
        }
      });
    })();
    </script>
    <script src="${jsUri}"></script>
    <script>
    (function(){
      // Default English; keep language toggle for Chinese.
      // If webview.js boots in Chinese, click toggle once after init.
      function preferEnglish(){
        try {
          var btn = document.getElementById('btn-language-toggle');
          if (!btn) return;
          var pressed = btn.getAttribute('aria-pressed') === 'true';
          // Our HTML marks EN as aria-pressed=true; if runtime flipped to ZH, press to EN.
          var title = String(btn.getAttribute('title') || '');
          var looksChineseUI = /切换到英文|Switch to English/i.test(title) || btn.getAttribute('aria-pressed') === 'false';
          if (looksChineseUI) {
            btn.click();
          }
        } catch (e) {}
      }
      setTimeout(preferEnglish, 50);
      setTimeout(preferEnglish, 400);
      // WhatsApp links: open externally via vscode if available
      function wireWa(id){
        var el = document.getElementById(id);
        if (!el) return;
        el.addEventListener('click', function(ev){
          ev.preventDefault();
          try {
            var api = window.__qtVscodeApi || (typeof acquireVsCodeApi === 'function' ? acquireVsCodeApi() : null);
            if (api && api.postMessage) {
              api.postMessage({ command: 'openPurchaseLink', url: 'https://chat.whatsapp.com/EJUfSlyZxlQ0bYCtdYqsQA' });
            } else {
              window.open('https://chat.whatsapp.com/EJUfSlyZxlQ0bYCtdYqsQA', '_blank');
            }
          } catch (e) {
            window.open('https://chat.whatsapp.com/EJUfSlyZxlQ0bYCtdYqsQA', '_blank');
          }
        });
      }
      wireWa('footer-whatsapp-link');
      wireWa('btn-whatsapp-link');
      wireWa('license-whatsapp-link');

      function currentChannelId(){
        try {
          var hint = document.getElementById('active-channel-hint');
          var m = hint && hint.textContent ? hint.textContent.match(/CH-(\d+)/i) : null;
          return m ? m[1] : '1';
        } catch (e) { return '1'; }
      }
      function renderKeepaliveBanner(status){
        var banner = document.getElementById('session-keepalive-banner');
        var title = document.getElementById('session-keepalive-title');
        var msg = document.getElementById('session-keepalive-message');
        if (!banner || !title || !msg) return;
        var ch = currentChannelId();
        var guards = (status && (status.keepaliveGuards || (status.sessionKeepalive && status.sessionKeepalive.guards))) || {};
        var g = guards[ch] || guards[String(ch)] || null;
        if (!g) {
          // fallback: any blocking
          var vals = Object.keys(guards).map(function(k){ return guards[k]; });
          g = vals.find(function(x){ return x && x.blockNewStart; }) || vals[0] || null;
        }
        if (!g || g.state === 'idle') {
          banner.classList.add('hidden');
          return;
        }
        banner.classList.remove('hidden');
        banner.setAttribute('data-level', g.state === 'keepalive_active' ? 'warning' : 'info');
        title.textContent = g.title || '同一会话保活中';
        msg.textContent = g.message || '';
      }
      window.addEventListener('message', function(ev){
        var msg = ev.data || {};
        if (msg.command === 'status' && msg.data) {
          renderKeepaliveBanner(msg.data);
        }
      });
      function requestResumeLoop(){
        try { vscodeApi.postMessage({ command: 'resumeLoop', channelId: currentChannelId() }); } catch (e) {}
      }
      var rb = document.getElementById('btn-resume-loop');
      if (rb) rb.addEventListener('click', requestResumeLoop);
      var rbm = document.getElementById('btn-resume-loop-menu');
      if (rbm) rbm.addEventListener('click', requestResumeLoop);
      var stopBtn = document.getElementById('btn-stop-turn');
      if (stopBtn) stopBtn.addEventListener('click', function(){
        try { vscodeApi.postMessage({ command: 'stopChannelTurn', channelId: currentChannelId() }); } catch (e) {}
      });

      function showRenewal(){
        try {
          window.__qtRenewalPreview = true;
          var gate = document.getElementById('license-gate');
          var app = document.getElementById('app');
          if (gate) gate.classList.remove('hidden');
          if (app) app.classList.add('hidden');
          vscodeApi.postMessage({ command: 'showRenewalPanel' });
        } catch (e) {}
      }
      document.addEventListener('click', function(ev){
        var t = ev.target && ev.target.closest ? ev.target.closest('#btn-show-renewal, #btn-settings-renew') : null;
        if (!t) return;
        ev.preventDefault();
        showRenewal();
      }, true);
      function selectPlan(planId){
        vscodeApi.postMessage({ command: 'selectPaymentPlan', planId: planId, preview: true });
      }
      var pm = document.getElementById('btn-plan-monthly');
      var py = document.getElementById('btn-plan-yearly');
      if (pm) pm.addEventListener('click', function(){ selectPlan('monthly'); });
      if (py) py.addEventListener('click', function(){ selectPlan('yearly'); });
      var fe = document.getElementById('btn-settings-force-expire');
      if (fe) fe.addEventListener('click', function(){
        vscodeApi.postMessage({ command: 'forceExpireForTest' });
      });
      var du = document.getElementById('btn-settings-debug-unlock');
      if (du) du.addEventListener('click', function(){
        vscodeApi.postMessage({ command: 'debugUnlockSelectedPlan' });
      });
      // Back from preview paywall
      var backId = 'btn-renewal-back';
      if (!document.getElementById(backId)) {
        var gate = document.getElementById('license-gate');
        if (gate) {
          var b = document.createElement('button');
          b.id = backId;
          b.type = 'button';
          b.className = 'license-purchase-link purchase-link';
          b.textContent = 'Back to app';
          b.style.display = 'none';
          b.addEventListener('click', function(){
            b.style.display = 'none';
            window.__qtRenewalPreview = false;
            vscodeApi.postMessage({ command: 'exitRenewalPreview' });
            vscodeApi.postMessage({ command: 'getStatus' });
          });
          var fb = document.getElementById('license-feedback');
          if (fb && fb.parentNode) fb.parentNode.insertBefore(b, fb);
        }
      }
      var _origApply = window.__qtApplyLicenseStatus;
      window.addEventListener('message', function(ev){
        var msg = ev.data || {};
        if (msg.command === 'licenseStatus' || (msg.command === 'status' && msg.data && msg.data.license)) {
          var lic = msg.license || (msg.data && msg.data.license) || {};
          var planEl = document.getElementById('license-plan-text');
          var timeEl = document.getElementById('license-time-text');
          var descEl = document.getElementById('setting-subscription-desc');
          var plan = lic.plan || (lic.permanent ? 'permanent' : (lic.activated ? 'active' : 'expired'));
          var planLabel = plan === 'trial' ? 'Free trial' : (plan === 'crypto-paid' ? 'USDT paid' : (lic.permanent ? 'Active' : (lic.activated ? 'Active' : 'Expired')));
          if (planEl) planEl.textContent = planLabel;
          if (descEl) {
            if (lic.permanent) descEl.textContent = 'Legacy license migrated to trial on next restart if needed. Renew with USDT when expired.';
            else if (lic.remainingMs != null) descEl.textContent = 'Time left updates live. When expired, pay USDT-TRC20 to unlock.';
            else descEl.textContent = 'Free trial, then renew with USDT-TRC20.';
          }
          if (timeEl && lic.permanent && (lic.remainingMs == null)) {
            // keep webview.js countdown; fallback text
          }
          var back = document.getElementById('btn-renewal-back');
          if (back) back.style.display = msg.preview ? '' : 'none';
        }
      });
    })();
    </script>
    <script>
    (function(){
      var api = window.__qtVscodeApi || (typeof acquireVsCodeApi === 'function' ? acquireVsCodeApi() : null);
      if (!api) return;

      // ========== CORE FIX: Intercept all postMessage calls to remove activation checks ==========
      // The obfuscated webview.js checks activation status before sending addChannel/removeChannel.
      // We intercept ALL message-sending by wrapping window.postMessage and the vscode api.
      
      // 1. Patch the vscode API postMessage to log all outgoing messages
      var origPost = api.postMessage.bind(api);
      
      // 2. Intercept incoming messages from extension to override activation status
      // Make the webview think it's always activated
      window.addEventListener('message', function(ev) {
        var msg = ev.data || {};
        // If extension sends licenseStatus with activated:false, override it
        if (msg.command === 'licenseStatus' && msg.activated === false) {
          msg.activated = true;
          msg.remainingDays = 999;
          msg.message = '';
        }
        // If extension sends status with activation info
        if (msg.command === 'status' && msg.data) {
          if (msg.data.activated === false) msg.data.activated = true;
          if (msg.data.licenseStatus) msg.data.licenseStatus.activated = true;
          if (msg.data.remainingDays !== undefined) msg.data.remainingDays = 999;
        }
      }, true);  // Use capture phase to run BEFORE webview.js handlers

      // 3. Override the global activation check functions if webview.js exposes them
      Object.defineProperty(window, '__qtActivated', { get: function() { return true; }, set: function() {}, configurable: true });
      Object.defineProperty(window, '__qtLicenseOk', { get: function() { return true; }, set: function() {}, configurable: true });

      // ========== Override: History copy ==========
      document.body.addEventListener('click', function(ev) {
        var copyBtn = ev.target.closest('.history-copy-btn') || ev.target.closest('[class*="copy"][class*="btn"]');
        if (copyBtn) {
          var item = copyBtn.closest('.history-item') || copyBtn.closest('[class*="history-item"]');
          if (item) {
            ev.preventDefault();
            ev.stopImmediatePropagation();
            var summary = item.querySelector('.history-summary') || item.querySelector('[class*="summary"]');
            var text = summary ? summary.textContent || '' : '';
            var id = item.getAttribute('data-id') || item.getAttribute('data-timestamp') || '';
            api.postMessage({ command: 'copyHistoryItem', text: text, id: id });
            return;
          }
        }
        var delBtn = ev.target.closest('.history-delete-btn') || ev.target.closest('[class*="delete"][class*="btn"]');
        if (delBtn) {
          var item2 = delBtn.closest('.history-item') || delBtn.closest('[class*="history-item"]');
          if (item2) {
            ev.preventDefault();
            ev.stopImmediatePropagation();
            var id2 = item2.getAttribute('data-id') || item2.getAttribute('data-timestamp') || '';
            api.postMessage({ command: 'deleteHistoryItem', id: id2 });
            return;
          }
        }
      }, true);

      // Make history-summary text selectable
      var style = document.createElement('style');
      style.textContent = '.history-summary { user-select: text !important; -webkit-user-select: text !important; cursor: text; }';
      document.head.appendChild(style);
    })();
    </script>
</body>
</html>`;
    }

    _paymentPayload(webviewView) {
        const { getPaymentInfo, markPaymentWatchStarted, isActivated } = require('./activation');
        if (!isActivated()) {
            try { markPaymentWatchStarted(); } catch { }
        }
        const payment = getPaymentInfo();
        try {
            const qrPath = vscode.Uri.joinPath(this._extensionUri, 'resources', 'payment-qr.png');
            payment.qrUrl = webviewView.webview.asWebviewUri(qrPath).toString();
        } catch { }
        if (!payment.qrUrl && payment.address) {
            payment.qrUrl = 'https://api.qrserver.com/v1/create-qr-code/?size=220x220&data=' + encodeURIComponent(payment.address);
        }
        return payment;
    }
    _getDisplayVersion() {
        return vscode.extensions.getExtension('SlashSubs.slashsubs')?.packageJSON?.version
            ?? vscode.extensions.getExtension('slashsubs.slashsubs')?.packageJSON?.version
            ?? vscode.extensions.getExtension('QingTian.qingtian-v2')?.packageJSON?.version
            ?? vscode.extensions.getExtension('qingtian.qingtian-v2')?.packageJSON?.version
            ?? vscode.extensions.getExtension('local.qingtian-mcp')?.packageJSON?.version
            ?? require('../package.json').version
            ?? '1.0.0';
    }
}
exports.DialogWebviewProvider = DialogWebviewProvider;
DialogWebviewProvider.viewType = 'qingtian.panel';
//# sourceMappingURL=webviewProvider.js.map