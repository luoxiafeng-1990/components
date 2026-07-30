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
exports.noteRuntimeConfigRefreshNeeded = noteRuntimeConfigRefreshNeeded;
exports.clearRuntimeConfigRefreshNeeded = clearRuntimeConfigRefreshNeeded;
exports.getMcpRuntimeStamp = getMcpRuntimeStamp;
exports.getMCPConfigStatus = getMCPConfigStatus;
exports.buildStatusPayload = buildStatusPayload;
const vscode = __importStar(require("vscode"));
const fs = __importStar(require("fs"));
const crypto = __importStar(require("crypto"));
const os = __importStar(require("os"));
const path = __importStar(require("path"));
const mcpServer_1 = require("./mcpServer");
const externalWebServer_1 = require("./externalWebServer");
const activation_1 = require("./activation");
let runtimeReloadHint = {
    active: false,
    reason: '',
    detectedAt: 0
};
function noteRuntimeConfigRefreshNeeded(reason) {
    runtimeReloadHint = {
        active: true,
        reason: reason || '',
        detectedAt: Date.now()
    };
}
function clearRuntimeConfigRefreshNeeded() {
    runtimeReloadHint = {
        active: false,
        reason: '',
        detectedAt: 0
    };
}
function getMcpRuntimeStamp(versionFallback) {
    try {
        const content = fs.readFileSync((0, mcpServer_1.getMCPServerPath)());
        return crypto.createHash('sha1').update(content).digest('hex').slice(0, 12);
    }
    catch {
        return versionFallback;
    }
}
function getCursorGlobalMCPConfigFile() {
    return path.join(os.homedir(), '.cursor', 'mcp.json');
}
function getWorkspaceMCPConfigFile() {
    const folders = vscode.workspace.workspaceFolders;
    if (!folders || folders.length === 0) {
        return null;
    }
    return path.join(folders[0].uri.fsPath, '.cursor', 'mcp.json');
}
function isNodeCommand(command) {
    const value = String(command || '').trim();
    if (!value) {
        return false;
    }
    const base = path.basename(value).toLowerCase();
    return value === 'node' || base === 'node' || base === 'node.exe';
}
function hasGlobalQingTianMCPEntries() {
    try {
        const configFile = getCursorGlobalMCPConfigFile();
        if (!fs.existsSync(configFile)) {
            return false;
        }
        const config = JSON.parse(fs.readFileSync(configFile, 'utf-8'));
        const servers = config?.mcpServers;
        if (!servers || typeof servers !== 'object') {
            return false;
        }
        return Object.keys(servers).some((key) => /^qtwx-mcp-\d+$/.test(key));
    }
    catch {
        return false;
    }
}
function getMCPConfigStatus(versionFallback) {
    try {
        const configFile = getWorkspaceMCPConfigFile();
        if (!configFile) {
            return { ok: false, reason: '请先打开工作区' };
        }
        const deployedScriptPath = (0, mcpServer_1.getMCPServerPath)();
        if (!fs.existsSync(deployedScriptPath)) {
            return { ok: false, reason: 'MCP 服务脚本未部署', configFile };
        }
        if (!fs.existsSync(configFile)) {
            return { ok: false, reason: '缺少工作区 .cursor/mcp.json', configFile };
        }
        let config;
        try {
            config = JSON.parse(fs.readFileSync(configFile, 'utf-8'));
        }
        catch {
            return { ok: false, reason: 'mcp.json 不是有效的 JSON', configFile };
        }
        const mcpServers = config?.mcpServers;
        if (!mcpServers || typeof mcpServers !== 'object') {
            return { ok: false, reason: 'mcp.json 缺少 mcpServers 配置', configFile };
        }
        // FREEZE: do not require stamp/script/queue to match deployed paths.
        // Strict matching caused "刷新配置" → mcp.json rewrite → Cursor config_changed stop.
        const chCount = (0, mcpServer_1.getChannelCount)();
        for (let i = 1; i <= chCount; i++) {
            const serverName = `qtwx-mcp-${i}`;
            const existing = mcpServers[serverName];
            if (!existing || !isNodeCommand(existing.command)) {
                return { ok: false, reason: `MCP 工具 ${serverName} 缺失或 command 无效`, configFile };
            }
            if (!existing.args?.[0] || typeof existing.args[0] !== 'string') {
                return { ok: false, reason: `MCP 工具 ${serverName} 缺少脚本路径`, configFile };
            }
            if (!existing.env?.QINGTIAN_SESSION || !existing.env?.QINGTIAN_QUEUE_ROOT) {
                return { ok: false, reason: `MCP 工具 ${serverName} 缺少 SESSION/QUEUE_ROOT`, configFile };
            }
        }
        if (hasGlobalQingTianMCPEntries()) {
            return {
                ok: true,
                configFile,
                warning: '全局 ~/.cursor/mcp.json 仍有旧 qtwx-mcp-*（可忽略；勿为清理而刷新配置）'
            };
        }
        return { ok: true, configFile };
    }
    catch (e) {
        return {
            ok: false,
            reason: '检查 MCP 配置时发生异常: ' + (e instanceof Error ? e.message : String(e))
        };
    }
}
function analyzeRuntimeChannels(status, versionFallback) {
    const expectedRuntimeStamp = getMcpRuntimeStamp(versionFallback);
    const staleChannelIds = [];
    const legacyChannelIds = [];
    let hasTrustedOnlineChannels = false;
    const channels = Object.fromEntries(Object.entries(status.channels || {}).map(([channelId, info]) => {
        const heartbeat = (0, mcpServer_1.readChannelHeartbeat)(channelId);
        const rawOnline = info.online === true;
        const runtimeStamp = heartbeat?.runtimeStamp || null;
        const missingRuntimeStamp = rawOnline && !runtimeStamp;
        const stampMismatch = rawOnline && Boolean(runtimeStamp) && runtimeStamp !== expectedRuntimeStamp;
        // Under mcp.json FREEZE, stamp mismatch is expected after extension upgrades
        // that redeploy scripts without rewriting mcp.json. Do NOT force offline.
        const staleRuntime = false;
        const trustedOnline = rawOnline;
        if (trustedOnline) {
            hasTrustedOnlineChannels = true;
        }
        if (stampMismatch) {
            staleChannelIds.push(channelId);
        }
        if (missingRuntimeStamp) {
            legacyChannelIds.push(channelId);
        }
        return [
            channelId,
            {
                ...info,
                online: trustedOnline,
                waitingActive: trustedOnline && info.waitingActive === true,
                runtimeStamp,
                staleRuntime,
                missingRuntimeStamp,
                stampMismatch
            }
        ];
    }));
    return {
        channels,
        hasTrustedOnlineChannels,
        staleChannelIds,
        legacyChannelIds
    };
}
function buildRuntimeNotice(runtime, versionFallback) {
    const configStatus = getMCPConfigStatus(versionFallback);
    if (!configStatus.ok) {
        return {
            code: 'config-mismatch',
            level: 'warning',
            title: 'MCP 配置待刷新',
            message: '检测到工作区 MCP 配置与插件版本不一致，或全局旧配置仍有残留。请先点击“刷新配置”，再重新加载窗口。'
        };
    }
    const affectedChannels = [...runtime.staleChannelIds, ...runtime.legacyChannelIds];
    if (affectedChannels.length > 0) {
        return null;
    }
    if (runtime.hasTrustedOnlineChannels) {
        clearRuntimeConfigRefreshNeeded();
        return null;
    }
    if (!runtimeReloadHint.active) {
        return null;
    }
    return null;
}
function buildStatusPayload(versionFallback) {
    const status = (0, mcpServer_1.getMCPStatus)();
    const runtime = analyzeRuntimeChannels(status, versionFallback);
    const cfg = vscode.workspace.getConfiguration('qingtian');
    const keepaliveGuards = status.keepaliveGuards || (0, mcpServer_1.getAllChannelKeepaliveGuards)();
    return {
        mcpConnected: status.mcpConnected,
        waiting: status.waiting,
        channelCount: status.channelCount,
        channels: runtime.channels,
        keepaliveGuards,
        sessionKeepalive: {
            anyActive: Object.values(keepaliveGuards || {}).some((g) => g && g.state !== 'idle'),
            anyBlocking: Object.values(keepaliveGuards || {}).some((g) => g && g.blockNewStart),
            guards: keepaliveGuards
        },
        license: (0, activation_1.getLicenseCountdownStatus)(),
        pluginSettings: (0, mcpServer_1.getPluginSettings)(),
        agentTeam: (0, mcpServer_1.getAgentTeamSnapshot)(),
        runtimeNotice: buildRuntimeNotice(runtime, versionFallback),
        keepaliveSettings: {
            keepaliveEnabled: cfg.get('keepaliveEnabled', true),
            keepaliveMinutes: cfg.get('keepaliveMinutes', 45)
        },
        notifySettings: {
            notifyOnReply: cfg.get('notifyOnReply', false)
        },
        bridgeSettings: {
            bridgeEnabled: cfg.get('bridgeEnabled', false),
            bridgeChannel: cfg.get('bridgeChannel', 1),
            bridgeBotToken: cfg.get('bridgeBotToken', ''),
            bridgeUseProxy: (0, mcpServer_1.getBridgeUseProxy)()
        },
        webServerInfo: (() => {
            const info = (0, externalWebServer_1.getWebServerInfo)();
            return {
                enabled: cfg.get('webServerEnabled', true),
                configuredPort: cfg.get('webServerPort', 3180),
                running: !!info?.running,
                url: info?.url || '',
                actualPort: info?.port || 0,
                clientCount: info?.clientCount || 0
            };
        })()
    };
}
//# sourceMappingURL=statusPayload.js.map