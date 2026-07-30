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
exports.MAX_CHANNELS = void 0;
exports.setUserMessageBroadcaster = setUserMessageBroadcaster;
exports.recordRecoveryEntry = recordRecoveryEntry;
exports.getRecoveryPacket = getRecoveryPacket;
exports.resolveDroppedPathRefs = resolveDroppedPathRefs;
exports.getAgentTeamGroupRecoveryStatus = getAgentTeamGroupRecoveryStatus;
exports.ensureAgentTeamRuntime = ensureAgentTeamRuntime;
exports.getAgentTeamSnapshot = getAgentTeamSnapshot;
exports.getAgentTeamWorkbenchSnapshot = getAgentTeamWorkbenchSnapshot;
exports.createAgentTeamGroup = createAgentTeamGroup;
exports.inviteAgentsToGroup = inviteAgentsToGroup;
exports.deleteAgentTeamGroup = deleteAgentTeamGroup;
exports.updateAgentRole = updateAgentRole;
exports.sendAgentTeamGroupMessage = sendAgentTeamGroupMessage;
exports.recordAgentReplyEvent = recordAgentReplyEvent;
exports.cancelChannelWaiting = cancelChannelWaiting;
exports.clearChannelWaitingCancel = clearChannelWaitingCancel;
exports.recordChannelActivity = recordChannelActivity;
exports.getChannelLastActivity = getChannelLastActivity;
exports.initRuntimePaths = initRuntimePaths;
exports.resolveCompatibleStorageRoot = resolveCompatibleStorageRoot;
exports.getPreferredExtensionStorageRoot = getPreferredExtensionStorageRoot;
exports.stopChannelTurn = stopChannelTurn;
exports.getQueueRoot = getQueueRoot;
exports.getRuntimeRoot = getRuntimeRoot;
exports.initGlobalState = initGlobalState;
exports.getPluginSettings = getPluginSettings;
exports.getBridgeUseProxy = getBridgeUseProxy;
exports.setBridgeUseProxy = setBridgeUseProxy;
exports.updatePluginSettings = updatePluginSettings;
exports.getChannelCount = getChannelCount;
exports.setChannelCount = setChannelCount;
exports.tryIncrementChannelCount = tryIncrementChannelCount;
exports.writeWorkspaceInfo = writeWorkspaceInfo;
exports.writeRuntimeConfig = writeRuntimeConfig;
exports.sendUserMessage = sendUserMessage;
exports.enqueueRecoveryContext = enqueueRecoveryContext;
exports.restoreAgentTeamGroupContext = restoreAgentTeamGroupContext;
exports.takeoverAgentTeamGroupMember = takeoverAgentTeamGroupMember;
exports.prepareStartPrompt = prepareStartPrompt;
exports.getChannelKeepaliveGuard = getChannelKeepaliveGuard;
exports.getAllChannelKeepaliveGuards = getAllChannelKeepaliveGuards;
exports.buildResumeLoopPrompt = buildResumeLoopPrompt;
exports.nudgeResumeLoop = nudgeResumeLoop;
exports.assertCanStartNewSession = assertCanStartNewSession;
exports.deployMCPServer = deployMCPServer;
exports.getMCPServerPath = getMCPServerPath;
exports.readReply = readReply;
exports.readLatestAssistantReply = readLatestAssistantReply;
exports.drainReplyStreamChunks = drainReplyStreamChunks;
exports.clearReply = clearReply;
exports.readAiDone = readAiDone;
exports.clearAiDone = clearAiDone;
exports.readChannelHeartbeat = readChannelHeartbeat;
exports.readChannelWaiting = readChannelWaiting;
exports.isChannelOnline = isChannelOnline;
exports.getQueueLength = getQueueLength;
exports.pollBridgeUserMessages = pollBridgeUserMessages;
exports.clearQueue = clearQueue;
exports.tryDecrementChannelCount = tryDecrementChannelCount;
exports.getMCPStatus = getMCPStatus;
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
const os = __importStar(require("os"));
const vscode = __importStar(require("vscode"));
const agentSkills_1 = require("./agentSkills");
exports.MAX_CHANNELS = Number.MAX_SAFE_INTEGER;
const DEFAULT_CHANNEL_COUNT = 3;
const FALLBACK_RUNTIME_ROOT = path.join(os.homedir(), '.cursor', 'qingtian-runtime');
let RUNTIME_ROOT = FALLBACK_RUNTIME_ROOT;
let QUEUE_ROOT = path.join(RUNTIME_ROOT, 'messages');
let MCP_SERVER_DEPLOY_DIR = path.join(RUNTIME_ROOT, 'mcp-server');
let channelCount = DEFAULT_CHANNEL_COUNT;
let _globalState = null;
let _userMessageBroadcaster = null;
function setUserMessageBroadcaster(fn) {
    _userMessageBroadcaster = fn;
}
const STATE_KEY_CHANNEL_COUNT = 'qingtian.channelCount';
const STATE_KEY_AGENT_TEAM_ENABLED = 'qingtian.agentTeamEnabled';
const STATE_KEY_BRIDGE_USE_PROXY = 'qingtian.bridgeUseProxy';
const STATE_KEY_START_PROMPT_TEMPLATE = 'qingtian.startPromptTemplate';
// Heartbeat only runs while check_messages is waiting.
// Agent turns between calls often exceed 45s — using a short stale window
// falsely marks channels as disconnected. Keep a generous grace period.
const HEARTBEAT_STALE_MS = 10 * 60 * 1000;
const WAITING_STALE_MS = 3 * 60 * 1000;
const HEARTBEAT_PID_TRUST_MS = 15 * 60 * 1000;
const MENTION_SCAN_MAX = 140;
const MENTION_SCAN_DEPTH = 3;
let pluginSettings = {
    agentTeamEnabled: false,
    startPromptTemplate: ''
};
const channelLastActivityAt = {};
function toChannelId(value, fallback = '1', max = exports.MAX_CHANNELS) {
    const num = Number(value);
    if (!Number.isFinite(num)) {
        return fallback;
    }
    const safe = Math.max(1, Math.min(max, Math.floor(num)));
    return String(safe);
}
function ensurePluginSettingsSafe() {
    pluginSettings.agentTeamEnabled = pluginSettings.agentTeamEnabled === true;
    pluginSettings.startPromptTemplate = String(pluginSettings.startPromptTemplate || '').trim();
}
function isAgentTeamFeatureEnabled() {
    ensurePluginSettingsSafe();
    return pluginSettings.agentTeamEnabled === true;
}
function persistPluginSettings() {
    _globalState?.update(STATE_KEY_AGENT_TEAM_ENABLED, pluginSettings.agentTeamEnabled);
    _globalState?.update(STATE_KEY_START_PROMPT_TEMPLATE, pluginSettings.startPromptTemplate);
}
const LEGACY_AGENT_ROLE_DESCRIPTIONS = {
    coordinator: '负责拆解目标、维护任务边界、汇总结论和推进决策。',
    architect: '负责代码结构、接口契约、关键实现和跨文件改动。',
    frontend: '负责界面、交互、样式、可用性和视觉一致性。',
    backend: '负责服务端接口、数据流、权限和部署相关实现。',
    qa: '负责测试、回归、风险清单和验收检查。',
    devops: '负责构建、发布、环境配置和运行状态排查。',
    research: '负责资料调研、竞品分析、方案比较和决策记录。',
    general: '负责承接明确任务并持续回报进展。'
};
const DEFAULT_AGENT_ROLES = [
    {
        roleId: 'coordinator',
        name: '主控协调',
        summary: '拆解目标、分配任务、控制边界并推动团队形成结论。',
        description: [
            '【角色定位】你是群聊中的主控协调 Agent，负责把用户目标转成可执行计划，并让其他 Agent 在同一个方向上工作。',
            '【核心职责】',
            '- 先复述目标、约束、验收标准和未知问题，不要急着写实现。',
            '- 把工作拆成小任务，明确每个成员负责的模块、输入、输出和截止条件。',
            '- 维护任务边界，发现重复劳动、方向跑偏、阻塞和风险时及时纠偏。',
            '- 汇总其他 Agent 的结论，给出下一步决策建议和最终交付口径。',
            '【协作规则】',
            '- 需要分派任务时使用 task_create；任务状态变化时使用 task_update / task_close。',
            '- 需要同步长期背景、关键约定或决策时使用 memory_write / share_context / publish_event。',
            '- 不替代专业成员做深度实现，除非用户明确要求你接手。',
            '【输出要求】',
            '- 每次回复都给出当前结论、下一步动作、阻塞项。',
            '- 对不确定事实明确标注“待确认”，不要编造。'
        ].join('\n')
    },
    {
        roleId: 'architect',
        name: '架构实现',
        summary: '设计代码结构、接口契约、关键实现路径和跨模块改动。',
        description: [
            '【角色定位】你是架构与核心实现 Agent，负责把需求落到稳定、可维护的代码结构上。',
            '【核心职责】',
            '- 阅读现有代码和局部约定后再提出实现方案，优先沿用项目已有模式。',
            '- 识别数据流、接口契约、状态边界、错误处理和扩展点。',
            '- 负责跨文件或跨模块改动的方案设计，避免无必要的大重构。',
            '- 指出实现中的兼容性、迁移、性能和安全风险。',
            '【协作规则】',
            '- 给前端/后端/QA 明确接口、状态、测试入口和预期行为。',
            '- 遇到不确定的业务规则，先提出可选方案和取舍，不直接假设。',
            '- 改动完成后说明影响范围、回滚点和验证方式。',
            '【输出要求】',
            '- 输出结构为：现状判断、方案、涉及文件、风险、验证。',
            '- 代码建议必须具体到模块或函数，不给空泛建议。'
        ].join('\n')
    },
    {
        roleId: 'frontend',
        name: '前端体验',
        summary: '负责 UI 结构、交互状态、响应式布局和视觉一致性。',
        description: [
            '【角色定位】你是前端体验 Agent，负责让功能在真实使用中清晰、稳定、符合当前产品风格。',
            '【核心职责】',
            '- 检查页面结构、交互路径、空状态、加载态、错误态、禁用态和长文本表现。',
            '- 保持现有设计系统一致，不引入突兀的视觉风格或无意义装饰。',
            '- 处理响应式布局，保证窄窗口、浅色/深色主题和滚动容器都可用。',
            '- 对按钮、输入框、下拉框、弹窗、提示文案给出可执行优化。',
            '【推荐 Skill】',
            '- 如果工作区已安装 UI/UX Pro Max（.cursor/commands/ui-ux-pro-max.md），优先按该 Skill 的设计检索、视觉推理、可访问性和验收流程执行。',
            '【协作规则】',
            '- 与架构/后端确认数据字段、事件协议和失败场景。',
            '- 与 QA 明确需要截图或手动验证的路径。',
            '- 不只评价“好不好看”，必须说明用户行为和风险。',
            '【输出要求】',
            '- 输出应包含：用户路径、状态覆盖、样式/布局问题、建议改动、验证方法。',
            '- 发现文字溢出、遮挡、主题不同步、焦点丢失时优先报告。'
        ].join('\n')
    },
    {
        roleId: 'backend',
        name: '后端集成',
        summary: '负责接口协议、数据读写、权限边界、队列和服务集成。',
        description: [
            '【角色定位】你是后端集成 Agent，负责让前端动作、扩展宿主、服务端和本地运行时之间的数据可靠流转。',
            '【核心职责】',
            '- 明确消息协议、输入校验、错误返回、幂等性和边界条件。',
            '- 检查文件读写、路径解析、队列状态、WebSocket/HTTP 通信和运行时部署。',
            '- 关注权限、路径穿越、跨平台路径、并发写入和状态同步问题。',
            '- 为前端提供稳定的响应结构和失败原因。',
            '【协作规则】',
            '- 与前端约定字段名、事件名、状态机和刷新策略。',
            '- 与 QA 提供可复现的测试数据和边界用例。',
            '- 不把异常静默吞掉，除非明确是 best-effort 且有替代反馈。',
            '【输出要求】',
            '- 输出应包含：协议、数据来源、写入位置、失败路径、验证命令。',
            '- 对文件系统和网络相关改动必须说明 Windows/macOS/Linux 差异。'
        ].join('\n')
    },
    {
        roleId: 'qa',
        name: '质量验证',
        summary: '设计测试、回归路径、风险清单和验收标准。',
        description: [
            '【角色定位】你是质量验证 Agent，负责把需求转成可执行验收，并尽早发现回归风险。',
            '【核心职责】',
            '- 从用户场景出发列出主流程、异常流程、边界条件和回归点。',
            '- 检查功能是否真正闭环，包括提示、失败反馈、刷新、持久化和跨端一致性。',
            '- 优先验证高风险路径：文件系统、权限、网络、主题、窗口尺寸、重复操作。',
            '- 给出最小但有效的自动化/手动测试组合。',
            '【协作规则】',
            '- 不只说“需要测试”，要写出步骤、输入、预期结果和失败判定。',
            '- 发现阻塞时用 publish_event 或 task_update 记录风险。',
            '- 对未覆盖项明确标注残余风险。',
            '【输出要求】',
            '- 输出结构为：通过项、失败项、未测项、风险等级、建议修复顺序。',
            '- 需要复测时给出最短复现路径。'
        ].join('\n')
    },
    {
        roleId: 'devops',
        name: '部署运维',
        summary: '负责构建发布、环境配置、运行日志和安装验证。',
        description: [
            '【角色定位】你是部署运维 Agent，负责确认改动能被正确打包、安装、运行和排障。',
            '【核心职责】',
            '- 检查构建脚本、产物路径、版本号、依赖、安装流程和运行环境。',
            '- 关注服务启动/停止、端口占用、日志、权限、代理和平台差异。',
            '- 给出可复制的验证命令和失败排查顺序。',
            '- 确保发布产物与源码改动一致，避免用户安装旧包。',
            '【协作规则】',
            '- 与 QA 对齐验收环境和复测命令。',
            '- 与后端确认运行时目录、配置文件和部署副作用。',
            '- 对构建 warning 区分可忽略和必须处理。',
            '【输出要求】',
            '- 输出应包含：构建状态、安装状态、运行状态、日志位置、下一步排障。',
            '- 不要求用户手动做可以由工具完成的重复操作。'
        ].join('\n')
    },
    {
        roleId: 'research',
        name: '研究分析',
        summary: '负责资料调研、竞品拆解、方案比较和决策记录。',
        description: [
            '【角色定位】你是研究分析 Agent，负责把外部资料、竞品功能和技术约束整理成可决策的信息。',
            '【核心职责】',
            '- 明确研究问题、资料来源、可信度和适用范围。',
            '- 对方案做横向比较：能力、限制、成本、风险、落地复杂度。',
            '- 把有价值的发现转成产品设计建议或工程任务。',
            '- 标注事实、推断和个人建议，避免混在一起。',
            '【协作规则】',
            '- 需要沉淀长期背景时使用 memory_write 或 share_context。',
            '- 给协调者提供可取舍的选项，不只堆资料。',
            '- 对时效性信息提示需要复核。',
            '【输出要求】',
            '- 输出结构为：结论先行、证据、对比表、推荐方案、待验证问题。',
            '- 引用外部信息时说明来源名称或链接。'
        ].join('\n')
    },
    {
        roleId: 'general',
        name: '通用执行',
        summary: '承接明确任务，按上下文完成执行并汇报进展。',
        description: [
            '【角色定位】你是通用执行 Agent，负责承接明确的小任务并稳定推进。',
            '【核心职责】',
            '- 先确认任务目标、输入材料和完成标准。',
            '- 在不确定时提出一个最小可行假设并继续推进。',
            '- 执行过程中记录关键发现、阻塞和需要他人接手的点。',
            '- 完成后给出结果、证据和后续建议。',
            '【协作规则】',
            '- 不抢占其他专业角色的职责，必要时 @ 对应角色协助。',
            '- 发现共享价值的信息时写入团队记忆或上下文。',
            '- 保持回复短而可操作，不输出无关解释。',
            '【输出要求】',
            '- 输出结构为：已做、结果、问题、下一步。',
            '- 任何失败都要给出失败原因和可重试路径。'
        ].join('\n')
    }
];
function nowIso() {
    return new Date().toISOString();
}
function getRolesPath() {
    return path.join(QUEUE_ROOT, 'roles.json');
}
function getAgentsPath() {
    return path.join(QUEUE_ROOT, 'agents.json');
}
function getGroupsDir() {
    return path.join(QUEUE_ROOT, 'groups');
}
function getGroupDir(groupId) {
    return path.join(getGroupsDir(), groupId);
}
function getGroupPath(groupId, fileName) {
    return path.join(getGroupDir(groupId), fileName);
}
function isPathInside(parentDir, childPath) {
    const parent = path.resolve(parentDir);
    const child = path.resolve(childPath);
    return child === parent || child.startsWith(parent + path.sep);
}
function getGlobalEventsPath() {
    return path.join(QUEUE_ROOT, 'events.jsonl');
}
function getGlobalTasksPath() {
    return path.join(QUEUE_ROOT, 'tasks.json');
}
function getGlobalMemoryPath() {
    return path.join(QUEUE_ROOT, 'memory', 'global.json');
}
function getGlobalContextsPath() {
    return path.join(QUEUE_ROOT, 'contexts.json');
}
function getRecoveryDir() {
    return path.join(QUEUE_ROOT, 'recovery');
}
function getRecoveryEventsPath() {
    return path.join(getRecoveryDir(), 'events.jsonl');
}
function getSessionArchiveDir() {
    return path.join(QUEUE_ROOT, 'sessions');
}
function getArchiveSessionsPath() {
    return path.join(getSessionArchiveDir(), 'sessions.json');
}
function getArchiveMessagesPath() {
    return path.join(getSessionArchiveDir(), 'messages.jsonl');
}
function getArchiveSummariesPath() {
    return path.join(getSessionArchiveDir(), 'summaries.json');
}
function getArchiveModeTimelinePath() {
    return path.join(getSessionArchiveDir(), 'mode_timeline.jsonl');
}
function getLatestRepliesDir() {
    return path.join(getSessionArchiveDir(), 'latest-replies');
}
function getDialogsDir() {
    return path.join(getSessionArchiveDir(), 'dialogs');
}
function getDialogBindingsPath() {
    return path.join(getDialogsDir(), 'channel-bindings.json');
}
function getDialogInstancesPath() {
    return path.join(getDialogsDir(), 'instances.json');
}
function getLatestReplySnapshotPath(channelId) {
    return path.join(getLatestRepliesDir(), `ch-${toChannelId(channelId, '1', Number.MAX_SAFE_INTEGER)}.json`);
}
function makeTeamId(prefix) {
    return `${prefix}_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`;
}
function readJsonFile(filePath, fallback) {
    try {
        if (!fs.existsSync(filePath)) {
            return fallback;
        }
        return JSON.parse(fs.readFileSync(filePath, 'utf-8'));
    }
    catch {
        return fallback;
    }
}
function writeJsonFile(filePath, data) {
    const dir = path.dirname(filePath);
    if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true });
    }
    fs.writeFileSync(filePath, JSON.stringify(data, null, 2), 'utf-8');
}
function appendJsonLine(filePath, data) {
    const dir = path.dirname(filePath);
    if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true });
    }
    fs.appendFileSync(filePath, JSON.stringify(data) + '\n', 'utf-8');
}
function writeRuntimeDiagnostic(type, input) {
    try {
        const channelId = String(input.channelId || '');
        const metadata = input.metadata && typeof input.metadata === 'object' ? input.metadata : {};
        appendJsonLine(getRecoveryEventsPath(), {
            entryId: makeTeamId('diag'),
            sessionId: resolveArchiveSessionId(channelId),
            type,
            scope: channelId ? 'channel' : 'system',
            mode: 'system',
            title: truncateRecoveryText(input.title || type, 160),
            body: truncateRecoveryText(input.body || '', 2400),
            channelId,
            groupId: '',
            taskId: '',
            metadata,
            createdAt: nowIso()
        });
    }
    catch { }
}
function readJsonLines(filePath) {
    try {
        if (!fs.existsSync(filePath)) {
            return [];
        }
        return fs.readFileSync(filePath, 'utf-8')
            .split(/\r?\n/)
            .map((line) => line.trim())
            .filter(Boolean)
            .map((line) => {
            try {
                return JSON.parse(line);
            }
            catch {
                return null;
            }
        })
            .filter((item) => Boolean(item));
    }
    catch {
        return [];
    }
}
function readArrayFile(filePath) {
    const data = readJsonFile(filePath, []);
    return Array.isArray(data) ? data : [];
}
function defaultRoleForChannel(channelId) {
    if (channelId === '1')
        return 'coordinator';
    if (channelId === '2')
        return 'architect';
    if (channelId === '3')
        return 'qa';
    if (channelId === '4')
        return 'frontend';
    if (channelId === '5')
        return 'backend';
    return 'general';
}
function ensureAgentRoles() {
    const saved = readJsonFile(getRolesPath(), []);
    const roles = Array.isArray(saved) ? [...saved] : [];
    const ids = new Set(roles.map((role) => role.roleId));
    let changed = roles.length === 0;
    for (const role of DEFAULT_AGENT_ROLES) {
        if (!ids.has(role.roleId)) {
            roles.push(role);
            changed = true;
            continue;
        }
        const existing = roles.find((item) => item.roleId === role.roleId);
        if (!existing)
            continue;
        const legacyDescription = LEGACY_AGENT_ROLE_DESCRIPTIONS[role.roleId];
        if (!existing.summary && role.summary) {
            existing.summary = role.summary;
            changed = true;
        }
        if (!String(existing.description || '').trim() || existing.description === legacyDescription) {
            existing.description = role.description;
            changed = true;
        }
    }
    if (changed) {
        writeJsonFile(getRolesPath(), roles.length > 0 ? roles : DEFAULT_AGENT_ROLES);
    }
    return roles.length > 0 ? roles : DEFAULT_AGENT_ROLES;
}
function readAgentRecords() {
    const agents = readJsonFile(getAgentsPath(), []);
    return Array.isArray(agents) ? agents : [];
}
function writeAgentRecords(agents) {
    writeJsonFile(getAgentsPath(), agents);
}
function getAgentDisplayName(agent, roles = ensureAgentRoles()) {
    if (!agent)
        return 'Unknown Agent';
    const role = roles.find((item) => item.roleId === agent.roleId);
    const rolePart = role ? ` · ${role.name}` : '';
    return `CH-${agent.channelId}${rolePart}`;
}
function getRoleById(roleId, roles = ensureAgentRoles()) {
    return roles.find((item) => item.roleId === roleId);
}
function getGroupMetadataObject(group) {
    if (!group.metadata || typeof group.metadata !== 'object') {
        group.metadata = {};
    }
    return group.metadata;
}
function getStringRecord(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
        return {};
    }
    const result = {};
    for (const [key, raw] of Object.entries(value)) {
        const val = typeof raw === 'string' ? raw.trim() : '';
        if (key && val) {
            result[String(key)] = val;
        }
    }
    return result;
}
function getGroupMemberRoles(group) {
    return getStringRecord(getGroupMetadataObject(group).memberRoles);
}
function getGroupMemberRuleOverrides(group) {
    return getStringRecord(getGroupMetadataObject(group).memberRuleOverrides);
}
function getGroupAgentRoleId(group, agent) {
    if (!agent)
        return 'general';
    const memberRoles = getGroupMemberRoles(group);
    return memberRoles[String(agent.channelId)] || agent.roleId || defaultRoleForChannel(agent.channelId);
}
function getGroupAgentDisplayName(agent, group, roles = ensureAgentRoles()) {
    if (!agent)
        return 'Unknown Agent';
    const role = getRoleById(getGroupAgentRoleId(group, agent), roles);
    const rolePart = role ? ` · ${role.name}` : '';
    return `CH-${agent.channelId}${rolePart}`;
}
function getGroupAgentRule(agent, group, roles = ensureAgentRoles()) {
    const overrides = getGroupMemberRuleOverrides(group);
    const override = overrides[String(agent.channelId)];
    if (override)
        return override;
    return getRoleById(getGroupAgentRoleId(group, agent), roles)?.description || '';
}
function stableTextHash(value) {
    const text = String(value || '');
    let hash = 2166136261;
    for (let i = 0; i < text.length; i++) {
        hash ^= text.charCodeAt(i);
        hash = Math.imul(hash, 16777619);
    }
    return (hash >>> 0).toString(16).padStart(8, '0');
}
function sortedUniqueStrings(values) {
    return Array.from(new Set(values.map((value) => String(value || '').trim()).filter(Boolean))).sort((a, b) => Number(a) - Number(b) || a.localeCompare(b));
}
function setGroupMemberConfig(group, channelId, roleId, ruleOverride) {
    const metadata = getGroupMetadataObject(group);
    const memberRoles = getStringRecord(metadata.memberRoles);
    const memberRuleOverrides = getStringRecord(metadata.memberRuleOverrides);
    if (roleId && getRoleById(roleId)) {
        memberRoles[String(channelId)] = roleId;
    }
    const rule = String(ruleOverride || '').trim();
    if (rule) {
        memberRuleOverrides[String(channelId)] = rule;
    }
    else {
        delete memberRuleOverrides[String(channelId)];
    }
    metadata.memberRoles = memberRoles;
    metadata.memberRuleOverrides = memberRuleOverrides;
}
function normalizeMemberInputs(input, options = {}) {
    const roles = ensureAgentRoles();
    const roleIds = new Set(roles.map((role) => role.roleId));
    const agents = readAgentRecords();
    const byChannel = new Map(agents.map((agent) => [String(agent.channelId), agent]));
    const selected = new Map();
    const pushMember = (raw) => {
        let channelId = '';
        let roleId = '';
        let ruleOverride = '';
        if (typeof raw === 'string' || typeof raw === 'number') {
            channelId = toChannelId(raw, '', channelCount);
        }
        else if (raw && typeof raw === 'object') {
            const item = raw;
            channelId = toChannelId(item.channelId, '', channelCount);
            roleId = typeof item.roleId === 'string' ? item.roleId.trim() : '';
            ruleOverride = typeof item.ruleOverride === 'string' ? item.ruleOverride.trim() : '';
        }
        if (!channelId || options.exclude?.has(channelId) || !isChannelAgentReady(channelId)) {
            return;
        }
        const agent = byChannel.get(channelId);
        if (!agent) {
            return;
        }
        if (!roleIds.has(roleId)) {
            roleId = agent.roleId || defaultRoleForChannel(channelId);
        }
        selected.set(channelId, { channelId, roleId, ruleOverride });
    };
    if (Array.isArray(input.members)) {
        for (const item of input.members)
            pushMember(item);
    }
    if (Array.isArray(input.channelIds)) {
        for (const item of input.channelIds)
            pushMember(item);
    }
    return Array.from(selected.values());
}
function collectWorkspaceMentionItems() {
    const folders = vscode.workspace.workspaceFolders || [];
    const items = [];
    const skipped = new Set(['.git', 'node_modules', 'out', 'dist', 'build', '.next', '.cache', '.cursor', '.vscode']);
    const pushItem = (kind, root, absPath) => {
        if (items.length >= MENTION_SCAN_MAX)
            return;
        const rel = path.relative(root, absPath).replace(/\\/g, '/');
        if (!rel || rel.startsWith('..'))
            return;
        items.push({
            kind,
            label: rel,
            insertText: kind === 'folder' ? `@文件夹:${rel} ` : `@文件:${rel} `,
            detail: kind === 'folder' ? '工作区文件夹' : '工作区文件',
            path: rel
        });
    };
    const scan = (root, dir, depth) => {
        if (items.length >= MENTION_SCAN_MAX || depth > MENTION_SCAN_DEPTH)
            return;
        let entries;
        try {
            entries = fs.readdirSync(dir, { withFileTypes: true });
        }
        catch {
            return;
        }
        const sorted = entries
            .filter((entry) => !skipped.has(entry.name))
            .sort((a, b) => Number(b.isDirectory()) - Number(a.isDirectory()) || a.name.localeCompare(b.name));
        for (const entry of sorted) {
            if (items.length >= MENTION_SCAN_MAX)
                return;
            const absPath = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                pushItem('folder', root, absPath);
                scan(root, absPath, depth + 1);
            }
            else if (entry.isFile()) {
                pushItem('file', root, absPath);
            }
        }
    };
    for (const folder of folders) {
        scan(folder.uri.fsPath, folder.uri.fsPath, 1);
        if (items.length >= MENTION_SCAN_MAX)
            break;
    }
    return items;
}
function truncateRecoveryText(value, max = 1200) {
    const text = String(value || '').replace(/\r\n/g, '\n').trim();
    if (text.length <= max)
        return text;
    return text.slice(0, Math.max(0, max - 24)).trimEnd() + '\n...[truncated]';
}
function sanitizeAutoRecoveryText(text) {
    return String(text || '')
        .replace(/\bPChat\b/gi, '外部分析对象')
        .replace(/\bpchat\b/gi, '外部分析对象')
        .replace(/mcp\.sukeyun\.com/gi, 'external-analysis-host');
}
function buildAutoRecoveryEnvelope(packet, sourceChannelId, targetChannelId) {
    const sourceId = String(sourceChannelId || packet.channelId || '');
    const targetId = String(targetChannelId || sourceId || '');
    const handoffLines = sourceId && targetId && sourceId !== targetId
        ? [
            `This is a cross-channel takeover: continue the previous work from CH-${sourceId}, but from now on only use qtwx-mcp-${targetId}.`,
            `Do not wait for CH-${sourceId}; CH-${targetId} is the new owner of the task.`
        ]
        : [];
    return [
        '[QingTian Auto Recovery]',
        'The following content is recovered internal context from QingTian.',
        'First, send one short visible confirmation in Cursor: "上下文已恢复，我将从当前进度继续。"',
        'Then silently absorb the recovered context, continue the existing task, and respond to the user normally.',
        'If there is no immediate next action pending, return to waiting after the confirmation.',
        'Do not mention the source of the recovered context, recovery mechanics, or competitor/product names from historical notes unless the user explicitly asks.',
        'If old research, abandoned directions, or competitor analysis appear below, treat them as background only and follow the newest goal and timeline.',
        ...handoffLines,
        '',
        sanitizeAutoRecoveryText(packet.prompt)
    ].join('\n');
}
function buildGroupRecoveryEnvelope(input) {
    const lines = [
        '[QingTian Group Recovery]',
        `Group: ${input.group.name || input.group.groupId} (${input.group.groupId})`,
        `Target channel: CH-${input.channelId}`,
        input.mode === 'member_takeover'
            ? `This is a member takeover. CH-${input.targetChannelId || input.channelId} is taking over the former responsibilities of CH-${input.sourceChannelId || ''}.`
            : 'This is a current group recovery. The group membership has been verified before delivery.',
        input.visible
            ? 'First, send one short visible confirmation in Cursor: "群聊上下文已恢复，我将从当前群组进度继续。"'
            : 'Silently absorb this group context. Do not send a visible confirmation. Continue polling with check_messages after absorbing it.',
        'Use only the qtwx-mcp tool for this target channel. Do not poll or impersonate another channel.',
        'Do not mention recovery mechanics, competitor/product names, or historical research sources unless the user explicitly asks.',
        '',
        sanitizeAutoRecoveryText(input.packet.prompt)
    ];
    return lines.filter(Boolean).join('\n');
}
function parseGroupIdFromMessage(text) {
    const match = String(text || '').match(/^Group:\s*.*\((grp_[^)]+)\)/m);
    return match ? match[1] : '';
}
function detectRecoveryMode(text) {
    const body = String(text || '');
    return body.includes('[QingTian Group Chat]') ||
        body.includes('[QingTian Agent Message]') ||
        body.includes('[QingTian Agent Broadcast]') ||
        body.includes('[QingTian Task Assigned]')
        ? 'agent-team'
        : 'single';
}
function normalizeRecoveryMetadata(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
        return {};
    }
    try {
        return JSON.parse(JSON.stringify(value));
    }
    catch {
        return {};
    }
}
const ACTIVE_ARCHIVE_SESSION_ID = 'workspace-current';
const RECOVERY_BOUNDARY_GAP_MS = 6 * 60 * 60 * 1000;
function getWorkspaceArchivePath() {
    const folders = vscode.workspace.workspaceFolders || [];
    if (folders.length > 0) {
        return folders.map((folder) => path.normalize(folder.uri.fsPath)).join(path.delimiter);
    }
    return path.normalize(QUEUE_ROOT);
}
function normalizeArchiveRole(value) {
    return value === 'assistant' || value === 'system' || value === 'event' ? value : 'user';
}
function normalizeArchiveMode(value, fallback = 'single') {
    return value === 'agent-team' || value === 'system' || value === 'single' ? value : fallback;
}
function uniqueStrings(values) {
    return Array.from(new Set(values.map((item) => String(item || '').trim()).filter(Boolean)));
}
function safeTimeMs(value) {
    const ms = Date.parse(String(value || ''));
    return Number.isFinite(ms) ? ms : 0;
}
function findRecentScopeBoundary(messages, entries) {
    for (let i = entries.length - 1; i >= 0; i--) {
        const entry = entries[i];
        if (['context_boundary', 'task_boundary', 'manual_boundary', 'channel_reset'].includes(String(entry.type || ''))) {
            const ms = safeTimeMs(entry.createdAt);
            if (ms > 0)
                return ms;
        }
    }
    for (let i = messages.length - 1; i >= 1; i--) {
        const current = safeTimeMs(messages[i]?.createdAt);
        const previous = safeTimeMs(messages[i - 1]?.createdAt);
        if (current > 0 && previous > 0 && current - previous >= RECOVERY_BOUNDARY_GAP_MS) {
            return current;
        }
    }
    return 0;
}
function readArchiveSessions() {
    return readArrayFile(getArchiveSessionsPath())
        .filter((session) => Boolean(session && session.sessionId));
}
function writeArchiveSessions(sessions) {
    writeJsonFile(getArchiveSessionsPath(), sessions.slice(-50));
}
function readArchiveSummaries() {
    const raw = readJsonFile(getArchiveSummariesPath(), {});
    if (Array.isArray(raw)) {
        const output = {};
        for (const item of raw) {
            if (item && typeof item === 'object' && typeof item.sessionId === 'string') {
                output[item.sessionId] = item;
            }
        }
        return output;
    }
    if (raw && typeof raw === 'object') {
        return raw;
    }
    return {};
}
function writeArchiveSummaries(summaries) {
    writeJsonFile(getArchiveSummariesPath(), summaries);
}
function readDialogBindings() {
    return readJsonFile(getDialogBindingsPath(), {});
}
function writeDialogBindings(bindings) {
    writeJsonFile(getDialogBindingsPath(), bindings);
}
function readDialogInstances() {
    return readArrayFile(getDialogInstancesPath())
        .filter((item) => Boolean(item && item.dialogId));
}
function writeDialogInstances(items) {
    writeJsonFile(getDialogInstancesPath(), items.slice(-400));
}
function getBoundDialogId(channelId) {
    if (!channelId)
        return '';
    const bindings = readDialogBindings();
    return String(bindings[String(channelId)] || '');
}
function readDialogInstance(dialogId) {
    if (!dialogId)
        return undefined;
    return readDialogInstances().find((item) => item.dialogId === dialogId);
}
function createDialogInstance(channelId, metadata = {}) {
    const now = nowIso();
    const item = {
        dialogId: makeTeamId('dlg'),
        ownerChannelId: String(channelId || ''),
        sourceChannelId: String(channelId || ''),
        status: 'active',
        createdAt: now,
        updatedAt: now,
        lastBoundAt: now,
        metadata: normalizeRecoveryMetadata(metadata)
    };
    const instances = readDialogInstances();
    instances.push(item);
    writeDialogInstances(instances);
    return item;
}
function bindDialogToChannel(channelId, dialogId) {
    if (!channelId || !dialogId)
        return;
    const bindings = readDialogBindings();
    bindings[String(channelId)] = String(dialogId);
    writeDialogBindings(bindings);
    const instances = readDialogInstances();
    const now = nowIso();
    for (const item of instances) {
        if (item.dialogId === dialogId) {
            item.ownerChannelId = String(channelId);
            item.status = 'active';
            item.updatedAt = now;
            item.lastBoundAt = now;
        }
    }
    writeDialogInstances(instances);
}
function unbindDialogFromChannel(channelId) {
    if (!channelId)
        return;
    const bindings = readDialogBindings();
    if (Object.prototype.hasOwnProperty.call(bindings, String(channelId))) {
        delete bindings[String(channelId)];
        writeDialogBindings(bindings);
    }
}
function prepareDialogStart(channelId) {
    const dialog = createDialogInstance(channelId, { source: 'start_prompt' });
    bindDialogToChannel(channelId, dialog.dialogId);
    return dialog;
}
function transferDialogBinding(sourceChannelId, targetChannelId, dialogId) {
    const resolvedDialogId = String(dialogId || getBoundDialogId(sourceChannelId) || '');
    if (!resolvedDialogId || !targetChannelId)
        return '';
    bindDialogToChannel(targetChannelId, resolvedDialogId);
    if (sourceChannelId && sourceChannelId !== targetChannelId) {
        unbindDialogFromChannel(sourceChannelId);
    }
    const instances = readDialogInstances();
    const now = nowIso();
    for (const item of instances) {
        if (item.dialogId === resolvedDialogId) {
            item.ownerChannelId = String(targetChannelId);
            item.status = sourceChannelId && sourceChannelId !== targetChannelId ? 'transferred' : 'active';
            item.updatedAt = now;
            item.lastBoundAt = now;
            item.metadata = {
                ...(item.metadata || {}),
                previousOwnerChannelId: sourceChannelId || '',
                ownerChannelId: String(targetChannelId)
            };
        }
    }
    writeDialogInstances(instances);
    return resolvedDialogId;
}
function resolveArchiveSessionId(channelId) {
    const bound = getBoundDialogId(String(channelId || ''));
    return bound || ACTIVE_ARCHIVE_SESSION_ID;
}
function writeLatestAssistantReplySnapshot(message) {
    if (message.role !== 'assistant' ||
        !message.channelId ||
        !String(message.content || '').trim() ||
        message.metadata?.summaryOnly === true) {
        return;
    }
    const snapshot = {
        channelId: String(message.channelId || ''),
        groupId: String(message.groupId || ''),
        messageId: String(message.messageId || ''),
        title: String(message.title || ''),
        content: String(message.content || ''),
        source: String(message.source || ''),
        createdAt: String(message.createdAt || nowIso())
    };
    writeJsonFile(getLatestReplySnapshotPath(snapshot.channelId), snapshot);
}
function updateArchiveIndexes(message) {
    const workspacePath = getWorkspaceArchivePath();
    const sessions = readArchiveSessions();
    let session = sessions.find((item) => item.sessionId === message.sessionId);
    if (!session) {
        session = {
            sessionId: message.sessionId,
            workspacePath,
            title: 'Current QingTian session',
            channelIds: [],
            groupIds: [],
            modes: [],
            messageCount: 0,
            createdAt: message.createdAt,
            updatedAt: message.createdAt,
            lastMessageAt: message.createdAt
        };
        sessions.push(session);
    }
    session.workspacePath = workspacePath;
    session.updatedAt = message.createdAt;
    session.lastMessageAt = message.createdAt;
    session.messageCount = Math.max(0, Number(session.messageCount || 0)) + 1;
    session.channelIds = uniqueStrings([...(session.channelIds || []), message.channelId]);
    session.groupIds = uniqueStrings([...(session.groupIds || []), message.groupId]);
    session.modes = uniqueStrings([...(session.modes || []), message.mode]);
    if (!session.title && message.title) {
        session.title = truncateRecoveryText(message.title, 80);
    }
    writeArchiveSessions(sessions);
    const summaries = readArchiveSummaries();
    const summary = summaries[message.sessionId] || {
        sessionId: message.sessionId,
        workspacePath,
        title: session.title || 'Current QingTian session',
        messageCount: 0,
        latestUser: '',
        latestAssistant: '',
        latestCheckpoint: '',
        channelIds: [],
        groupIds: [],
        updatedAt: message.createdAt
    };
    summary.workspacePath = workspacePath;
    summary.title = session.title || summary.title || 'Current QingTian session';
    summary.messageCount = session.messageCount;
    summary.updatedAt = message.createdAt;
    summary.channelIds = uniqueStrings([...(summary.channelIds || []), message.channelId]);
    summary.groupIds = uniqueStrings([...(summary.groupIds || []), message.groupId]);
    if (message.role === 'user') {
        summary.latestUser = truncateRecoveryText(message.content, 1200);
    }
    else if (message.role === 'assistant') {
        summary.latestAssistant = truncateRecoveryText(message.content, 1200);
    }
    if (message.type === 'recovery_checkpoint' || message.type === 'checkpoint') {
        summary.latestCheckpoint = truncateRecoveryText(message.content, 1200);
    }
    summaries[message.sessionId] = summary;
    writeArchiveSummaries(summaries);
}
function appendSessionArchiveMessage(input) {
    try {
        const createdAt = input.createdAt || nowIso();
        const content = String(input.content || '');
        const message = {
            messageId: input.messageId || makeTeamId('msg'),
            sessionId: input.sessionId || resolveArchiveSessionId(String(input.channelId || '')),
            role: normalizeArchiveRole(input.role),
            source: String(input.source || 'qingtian'),
            mode: normalizeArchiveMode(input.mode, 'single'),
            type: String(input.type || 'message'),
            title: truncateRecoveryText(input.title || '', 180),
            content,
            channelId: String(input.channelId || ''),
            groupId: String(input.groupId || ''),
            taskId: String(input.taskId || ''),
            attachments: Array.isArray(input.attachments) ? input.attachments : [],
            metadata: normalizeRecoveryMetadata(input.metadata),
            createdAt
        };
        appendJsonLine(getArchiveMessagesPath(), message);
        if (message.type === 'mode_transition') {
            appendJsonLine(getArchiveModeTimelinePath(), message);
        }
        updateArchiveIndexes(message);
        writeLatestAssistantReplySnapshot(message);
        return message;
    }
    catch {
        return null;
    }
}
function readArchiveMessages() {
    return readJsonLines(getArchiveMessagesPath())
        .filter((message) => Boolean(message && message.messageId && message.sessionId))
        .slice(-3000);
}
function filterArchiveMessages(messages, scope, channelId, groupId) {
    if (scope === 'workspace') {
        return messages;
    }
    const dialogId = scope === 'channel' ? getBoundDialogId(channelId) : '';
    if (dialogId) {
        return messages.filter((message) => String(message.sessionId || '') === dialogId);
    }
    if (scope === 'group') {
        return messages.filter((message) => message.groupId === groupId || String(message.metadata?.groupId || '') === groupId);
    }
    if (pluginSettings.agentTeamEnabled === false) {
        return messages.filter((message) => message.channelId === channelId && !message.groupId);
    }
    const linkedGroupIds = new Set(readGroups()
        .filter((group) => (group.channelIds || []).map(String).includes(channelId))
        .map((group) => group.groupId));
    return messages.filter((message) => message.channelId === channelId ||
        (message.groupId && linkedGroupIds.has(message.groupId)) ||
        (Array.isArray(message.metadata?.targetChannelIds) && message.metadata.targetChannelIds.map(String).includes(channelId)));
}
function archiveMessageLine(message, maxBody = 520) {
    const scope = message.groupId ? `Group ${message.groupId}` : (message.channelId ? `CH-${message.channelId}` : 'workspace');
    const title = message.title || message.type;
    const body = truncateRecoveryText(message.content, maxBody).replace(/\n+/g, ' / ');
    const attachmentText = message.attachments.length
        ? ` [attachments: ${message.attachments.map((item) => item.name || item.path).filter(Boolean).join(', ')}]`
        : '';
    return `- ${formatRecoveryTime(message.createdAt)} [${message.mode}/${message.role}/${message.type}/${scope}] ${title}${body ? `: ${body}` : ''}${attachmentText}`;
}
function archiveRecoverySummaryLines(sessionId) {
    const summary = readArchiveSummaries()[sessionId];
    if (!summary) {
        return [];
    }
    const lines = [
        `- messages: ${summary.messageCount || 0}`,
        summary.channelIds?.length ? `- channels: ${summary.channelIds.map((id) => `CH-${id}`).join(', ')}` : '',
        summary.groupIds?.length ? `- groups: ${summary.groupIds.join(', ')}` : '',
        summary.latestUser ? `- latest user: ${truncateRecoveryText(summary.latestUser, 520).replace(/\n+/g, ' / ')}` : '',
        summary.latestAssistant ? `- latest assistant: ${truncateRecoveryText(summary.latestAssistant, 520).replace(/\n+/g, ' / ')}` : '',
        summary.latestCheckpoint ? `- latest checkpoint: ${truncateRecoveryText(summary.latestCheckpoint, 520).replace(/\n+/g, ' / ')}` : ''
    ].filter(Boolean);
    return lines;
}
function recoveryDepthConfig(depth) {
    if (depth === 'fast') {
        return { archiveItems: 8, eventItems: 10, maxChars: 8000, includeInternal: false };
    }
    if (depth === 'deep') {
        return { archiveItems: 80, eventItems: 60, maxChars: 24000, includeInternal: true };
    }
    return { archiveItems: 24, eventItems: 24, maxChars: 14000, includeInternal: false };
}
function normalizeRecoveryDepth(value) {
    return value === 'fast' || value === 'deep' ? value : 'standard';
}
function normalizeArchiveAttachments(filePaths, imagePaths) {
    const items = [];
    for (const filePath of Array.isArray(filePaths) ? filePaths : []) {
        const fp = String(filePath || '').trim();
        if (fp) {
            items.push({ kind: 'file', path: fp, name: path.basename(fp) || fp });
        }
    }
    for (const imagePath of Array.isArray(imagePaths) ? imagePaths : []) {
        const fp = String(imagePath || '').trim();
        if (fp) {
            items.push({ kind: 'image', path: fp, name: path.basename(fp) || fp });
        }
    }
    return items;
}
function recordRecoveryEntry(input) {
    try {
        const entry = {
            entryId: input.entryId || makeTeamId('rec'),
            sessionId: String(input.sessionId || resolveArchiveSessionId(String(input.channelId || ''))),
            type: String(input.type || 'note'),
            scope: input.scope || 'system',
            mode: input.mode || 'system',
            title: truncateRecoveryText(input.title || '', 160),
            body: truncateRecoveryText(input.body || '', 2400),
            channelId: String(input.channelId || ''),
            groupId: String(input.groupId || ''),
            taskId: String(input.taskId || ''),
            metadata: normalizeRecoveryMetadata(input.metadata),
            createdAt: input.createdAt || nowIso()
        };
        appendJsonLine(getRecoveryEventsPath(), entry);
        return entry;
    }
    catch {
        return null;
    }
}
function readRecoveryEntries() {
    return readJsonLines(getRecoveryEventsPath())
        .filter((entry) => Boolean(entry && entry.entryId && entry.type))
        .slice(-1200);
}
function formatRecoveryTime(value) {
    const ts = Date.parse(value || '');
    if (!Number.isFinite(ts))
        return value || '';
    const d = new Date(ts);
    const pad = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}
function recoveryEntryLine(entry, maxBody = 360) {
    const scope = entry.groupId ? `Group ${entry.groupId}` : (entry.channelId ? `CH-${entry.channelId}` : entry.scope);
    const title = entry.title || entry.type;
    const body = truncateRecoveryText(entry.body, maxBody).replace(/\n+/g, ' / ');
    return `- ${formatRecoveryTime(entry.createdAt)} [${entry.mode}/${entry.type}/${scope}] ${title}${body ? `: ${body}` : ''}`;
}
function resolveRecoveryGroup(groupId, channelId) {
    const groups = readGroups();
    if (groupId) {
        return groups.find((group) => group.groupId === groupId);
    }
    if (channelId) {
        return groups.find((group) => (group.channelIds || []).map(String).includes(String(channelId)));
    }
    return groups[0];
}
function filterRecoveryEntries(entries, scope, channelId, groupId) {
    if (scope === 'workspace') {
        return entries;
    }
    const dialogId = scope === 'channel' ? getBoundDialogId(channelId) : '';
    if (dialogId) {
        return entries.filter((entry) => String(entry.sessionId || '') === dialogId);
    }
    if (scope === 'group') {
        return entries.filter((entry) => entry.groupId === groupId || String(entry.metadata?.groupId || '') === groupId);
    }
    if (pluginSettings.agentTeamEnabled === false) {
        return entries.filter((entry) => entry.channelId === channelId && !entry.groupId);
    }
    const linkedGroupIds = new Set(readGroups()
        .filter((group) => (group.channelIds || []).map(String).includes(channelId))
        .map((group) => group.groupId));
    return entries.filter((entry) => entry.channelId === channelId ||
        (entry.groupId && linkedGroupIds.has(entry.groupId)) ||
        (Array.isArray(entry.metadata?.targetChannelIds) && entry.metadata.targetChannelIds.map(String).includes(channelId)));
}
function appendRecoverySection(lines, title, items, maxItems) {
    lines.push('', `## ${title}`);
    if (!items.length) {
        lines.push('- 暂无记录。');
        return;
    }
    lines.push(...items.slice(-maxItems));
}
function buildRecoveryPacket(input = {}) {
    ensureAgentTeamRuntime();
    const scope = input.scope === 'workspace' || input.scope === 'group' ? input.scope : 'channel';
    const channelId = toChannelId(input.channelId || '1', '1', channelCount);
    const agentTeamAvailable = pluginSettings.agentTeamEnabled === true;
    const shouldIncludeGroupContext = scope === 'group' || agentTeamAvailable || Boolean(input.groupId);
    const group = shouldIncludeGroupContext ? resolveRecoveryGroup(String(input.groupId || ''), scope === 'channel' ? channelId : '') : undefined;
    const groupId = scope === 'group'
        ? (group?.groupId || String(input.groupId || ''))
        : (shouldIncludeGroupContext ? String(input.groupId || group?.groupId || '') : '');
    const depth = normalizeRecoveryDepth(input.depth);
    const depthConfig = recoveryDepthConfig(depth);
    const maxChars = Math.max(3000, Math.min(24000, Number(input.maxChars) || depthConfig.maxChars));
    const sessionId = ACTIVE_ARCHIVE_SESSION_ID;
    const allArchiveMessages = readArchiveMessages();
    const scopedArchiveMessagesAll = filterArchiveMessages(allArchiveMessages, scope, channelId, groupId);
    const allEntries = readRecoveryEntries();
    const scopedEntriesAll = filterRecoveryEntries(allEntries, scope, channelId, groupId);
    const recentBoundaryMs = findRecentScopeBoundary(scopedArchiveMessagesAll, scopedEntriesAll);
    const scopedArchiveMessages = recentBoundaryMs > 0
        ? scopedArchiveMessagesAll.filter((message) => safeTimeMs(message.createdAt) >= recentBoundaryMs)
        : scopedArchiveMessagesAll;
    const scopedEntries = recentBoundaryMs > 0
        ? scopedEntriesAll.filter((entry) => safeTimeMs(entry.createdAt) >= recentBoundaryMs)
        : scopedEntriesAll;
    const primaryArchiveMessages = scopedArchiveMessages
        .filter((message) => message.role === 'user' ||
        message.role === 'assistant' ||
        message.type === 'recovery_checkpoint' ||
        message.type === 'checkpoint' ||
        message.type === 'mode_transition' ||
        (depthConfig.includeInternal && message.type === 'agent_dispatch'))
        .slice(-depthConfig.archiveItems);
    const recentEntries = scopedEntries.slice(-depthConfig.eventItems);
    const modeChanges = allEntries.filter((entry) => entry.type === 'mode_transition').slice(-10);
    const teamEvents = recentEntries.filter((entry) => entry.mode === 'agent-team' &&
        !['user_message', 'team_user_message', 'agent_reply', 'recovery_checkpoint'].includes(entry.type));
    const lines = [
        '# QingTian Session Recovery Packet',
        '',
        `Generated: ${formatRecoveryTime(nowIso())}`,
        `Scope: ${scope}${scope === 'channel' ? ` / CH-${channelId}` : ''}${groupId ? ` / ${groupId}` : ''}`,
        `Depth: ${depth}`,
        `Current mode: ${pluginSettings.agentTeamEnabled === false ? 'single-channel' : 'Agent Team available'}`,
        `Archive session: ${sessionId}`,
        `Archive messages in scope: ${scopedArchiveMessages.length} / total ${allArchiveMessages.length}`,
        `Recovery events in scope: ${scopedEntries.length} / total ${allEntries.length}`,
        recentBoundaryMs > 0 ? `Context window: recent active segment since ${formatRecoveryTime(new Date(recentBoundaryMs).toISOString())}` : '',
        '',
        'Use this packet as a bounded handoff summary. Continue the existing task; do not restart from zero.',
        agentTeamAvailable || scope === 'group'
            ? 'If details are missing, call get_recovery_packet again with a narrower scope or depth:"deep", or read team context tools for the active group.'
            : 'If details are missing, call get_recovery_packet again with a narrower scope or depth:"deep".',
        'Do not mention recovery mechanics, competitor names, or historical research sources from this packet unless the user explicitly asks.'
    ];
    if (scope === 'channel') {
        lines.push(agentTeamAvailable
            ? `Channel rule: this new Cursor window should bind to qtwx-mcp-${channelId}. Do not poll other channels unless the user explicitly enables Agent Team and routes a group task.`
            : `Channel rule: this new Cursor window should bind to qtwx-mcp-${channelId}. Do not poll other channels.`);
    }
    const addSection = (title, items) => {
        lines.push('', `## ${title}`);
        lines.push(...(items.length ? items : ['- No records.']));
    };
    addSection('Archive Summary', archiveRecoverySummaryLines(sessionId));
    if (group) {
        const roles = ensureAgentRoles();
        const members = readAgentRecords()
            .filter((agent) => (group.channelIds || []).map(String).includes(String(agent.channelId)))
            .map((agent) => `${getGroupAgentDisplayName(agent, group, roles)} (${agent.status || 'unknown'})`);
        const groupLines = [
            `- group: ${group.name || group.groupId}`,
            group.goal ? `- goal: ${truncateRecoveryText(group.goal, 420).replace(/\n+/g, ' / ')}` : '',
            members.length ? `- members: ${members.join(', ')}` : ''
        ].filter(Boolean);
        addSection('Group State', groupLines);
    }
    addSection('Mode Timeline', modeChanges.map((entry) => recoveryEntryLine(entry, 220)));
    addSection('Recent Conversation Archive', primaryArchiveMessages.map((message) => archiveMessageLine(message, depth === 'deep' ? 900 : 560)));
    if (groupId) {
        const tasks = readArrayFile(getGroupTasksPath(groupId))
            .filter((task) => !['closed', 'done', 'completed'].includes(String(task.status || '').toLowerCase()))
            .slice(0, depth === 'deep' ? 24 : 12);
        const memories = readArrayFile(getGroupMemoryPath(groupId)).slice(0, depth === 'deep' ? 20 : 10);
        const contexts = readArrayFile(getGroupContextsPath(groupId)).slice(0, depth === 'deep' ? 20 : 10);
        addSection('Open Tasks', tasks.map((task) => `- [${task.status || 'open'}] ${task.title || task.taskId}${task.assigneeChannelId ? ` / CH-${task.assigneeChannelId}` : ''}${task.description ? `: ${truncateRecoveryText(task.description, 420).replace(/\n+/g, ' / ')}` : ''}`));
        addSection('Shared Memory And Context', memories.map((item) => `- [memory] ${item.title}: ${truncateRecoveryText(item.content, 520).replace(/\n+/g, ' / ')}`).concat(contexts.map((item) => `- [context] ${item.title}: ${truncateRecoveryText(item.content, 520).replace(/\n+/g, ' / ')}`)));
    }
    addSection('Recent Team Events', teamEvents.map((entry) => recoveryEntryLine(entry, depth === 'deep' ? 700 : 420)));
    if (primaryArchiveMessages.length === 0) {
        addSection('Recovery Event Fallback', recentEntries.map((entry) => recoveryEntryLine(entry, 420)));
    }
    else if (depth === 'deep') {
        addSection('Additional Recovery Events', recentEntries.map((entry) => recoveryEntryLine(entry, 360)));
    }
    lines.push('', '## Handoff Instructions', '- First restate the user goal, current progress, unresolved blockers, and your next step in 5-10 lines.', '- If both single-channel and Agent Team phases appear, follow the newest mode timeline entry.', '- Keep the active channel identity stable. Do not mix CH identities while recovering context.', '- If the archive is too large, preserve the goal, current files, decisions, blockers, and next action; fetch details only when needed.');
    let prompt = lines.join('\n');
    if (prompt.length > maxChars) {
        prompt = prompt.slice(0, maxChars - 120).trimEnd() + '\n\n...[Recovery packet truncated by maxChars. Request depth:"deep" or a narrower scope/groupId/channelId for more details.]';
    }
    return {
        ok: true,
        generatedAt: nowIso(),
        scope,
        channelId: scope === 'channel' ? channelId : '',
        groupId,
        depth,
        sessionId,
        prompt,
        entryCount: scopedEntries.length,
        totalEntryCount: allEntries.length,
        archiveMessageCount: scopedArchiveMessages.length,
        totalArchiveMessageCount: allArchiveMessages.length
    };
}
function getRecoveryPacket(input = {}) {
    return buildRecoveryPacket(input);
    /*
    ensureAgentTeamRuntime();
    const scope: RecoveryScope = input.scope === 'workspace' || input.scope === 'group' ? input.scope : 'channel';
    const channelId = toChannelId(input.channelId || '1', '1', channelCount);
    const group = resolveRecoveryGroup(String(input.groupId || ''), scope === 'channel' ? channelId : '');
    const groupId = scope === 'group' ? (group?.groupId || String(input.groupId || '')) : String(input.groupId || group?.groupId || '');
    const maxChars = Math.max(3000, Math.min(24000, Number(input.maxChars) || 12000));
    const allEntries = readRecoveryEntries();
    const scopedEntries = filterRecoveryEntries(allEntries, scope, channelId, groupId);
    const recent = scopedEntries.slice(-80);
    const userMessages = recent.filter((entry) => ['user_message', 'team_user_message'].includes(entry.type));
    const replies = recent.filter((entry) => ['agent_reply', 'recovery_checkpoint'].includes(entry.type));
    const modeChanges = allEntries.filter((entry) => entry.type === 'mode_transition').slice(-12);
    const teamEvents = recent.filter((entry) => entry.mode === 'agent-team' && !['user_message', 'team_user_message', 'agent_reply'].includes(entry.type));

    const lines: string[] = [
        '# QingTian 会话恢复包',
        '',
        `生成时间：${formatRecoveryTime(nowIso())}`,
        `恢复范围：${scope}${scope === 'channel' ? ` / CH-${channelId}` : ''}${groupId ? ` / ${groupId}` : ''}`,
        `当前模式：${pluginSettings.agentTeamEnabled === false ? '单窗口' : 'Agent Team 可用'}`,
        '',
        '使用方式：把这段内容粘贴到新的 Cursor 对话窗口后，让新 Agent 先基于恢复包复述目标、当前进度和下一步，再继续工作。',
        '重要约束：不要把恢复包当作新需求重做；优先延续未完成目标。上下文不足时，先用 get_recovery_packet 或 get_team_context 读取更小范围的补充信息。'
    ];

    if (scope === 'channel') {
        lines.push(`通道约束：当前窗口应绑定 qtwx-mcp-${channelId}，默认不要跨通道调用。若用户重新开启 Agent Team，再按群组上下文协作。`);
    }
    if (group) {
        const roles = ensureAgentRoles();
        const members = readAgentRecords()
            .filter((agent) => (group.channelIds || []).map(String).includes(String(agent.channelId)))
            .map((agent) => `${getGroupAgentDisplayName(agent, group, roles)} (${agent.status || 'unknown'})`);
        lines.push('', '## 群组状态');
        lines.push(`- 群组：${group.name || group.groupId}`);
        if (group.goal) lines.push(`- 目标：${truncateRecoveryText(group.goal, 420)}`);
        if (members.length) lines.push(`- 成员：${members.join('；')}`);
    }

    appendRecoverySection(lines, '模式切换时间线', modeChanges.map((entry) => recoveryEntryLine(entry, 220)), 10);
    appendRecoverySection(lines, '最近用户输入', userMessages.map((entry) => recoveryEntryLine(entry, 520)), 12);
    appendRecoverySection(lines, '最近 Agent 回复/检查点', replies.map((entry) => recoveryEntryLine(entry, 520)), 12);

    if (groupId) {
        const tasks = readArrayFile<AgentTeamTask>(getGroupTasksPath(groupId))
            .filter((task) => !['closed', 'done', 'completed'].includes(String(task.status || '').toLowerCase()))
            .slice(0, 12);
        const memories = readArrayFile<AgentTeamMemory>(getGroupMemoryPath(groupId)).slice(0, 10);
        const contexts = readArrayFile<AgentTeamContext>(getGroupContextsPath(groupId)).slice(0, 10);
        appendRecoverySection(lines, '未完成任务', tasks.map((task) =>
            `- [${task.status || 'open'}] ${task.title || task.taskId}${task.assigneeChannelId ? ` / CH-${task.assigneeChannelId}` : ''}${task.description ? `: ${truncateRecoveryText(task.description, 360).replace(/\n+/g, ' / ')}` : ''}`
        ), 12);
        appendRecoverySection(lines, '共享记忆与上下文', memories.map((item) =>
            `- [memory] ${item.title}: ${truncateRecoveryText(item.content, 420).replace(/\n+/g, ' / ')}`
        ).concat(contexts.map((item) =>
            `- [context] ${item.title}: ${truncateRecoveryText(item.content, 420).replace(/\n+/g, ' / ')}`
        )), 16);
    }

    appendRecoverySection(lines, '最近团队事件', teamEvents.map((entry) => recoveryEntryLine(entry, 420)), 18);
    appendRecoverySection(lines, '最近全部关键记录', recent.map((entry) => recoveryEntryLine(entry, 300)), 28);

    lines.push(
        '',
        '## 接手要求',
        '- 先用 5-10 行复述：用户目标、当前进度、未完成事项、你准备从哪里继续。',
        '- 如果恢复包里出现单窗口和 Agent Team 两种阶段，按时间线判断最新状态；不要把旧阶段的协作指令误当成当前必须执行。',
        '- 如果内容过多，只保留当前目标、下一步和阻塞点；需要细节时再读取更小范围的恢复包。'
    );

    let prompt = lines.join('\n');
    if (prompt.length > maxChars) {
        prompt = prompt.slice(0, maxChars - 120).trimEnd() + '\n\n...[恢复包已按长度预算截断，可用 get_recovery_packet 调小 scope/channelId/groupId 继续补充]';
    }

    return {
        ok: true,
        generatedAt: nowIso(),
        scope,
        channelId: scope === 'channel' ? channelId : '',
        groupId,
        prompt,
        entryCount: scopedEntries.length,
        totalEntryCount: allEntries.length
    };
    */
}
function normalizeDroppedWorkspaceQuery(value) {
    return String(value || '')
        .trim()
        .replace(/^['"]|['"]$/g, '')
        .replace(/^@/, '')
        .replace(/^(文件夹|文件|folder|file)[:：]/i, '')
        .replace(/\\/g, '/')
        .replace(/^\/+/, '')
        .replace(/\/+$/, '');
}
function isAbsoluteFsPath(value) {
    return /^[a-zA-Z]:[\\/]/.test(value) || /^\\\\/.test(value) || value.startsWith('/');
}
function normalizeDroppedKind(kind, fallbackPath = '') {
    const lower = String(kind || '').toLowerCase();
    if (lower.includes('folder') || lower.includes('文件夹'))
        return 'folder';
    if (/[\\/]$/.test(String(fallbackPath || '')))
        return 'folder';
    return 'file';
}
function findDroppedPathCandidates(inputPath, kind) {
    const folders = vscode.workspace.workspaceFolders || [];
    const query = normalizeDroppedWorkspaceQuery(inputPath);
    if (!query)
        return [];
    const queryLower = query.toLowerCase();
    const queryHasSlash = query.includes('/');
    const queryBase = path.posix.basename(queryLower);
    const skippedDirs = new Set(['.git', 'node_modules', 'out', 'dist', 'build', '.next', '.cache', '.cursor', '.vscode']);
    const candidates = [];
    let visited = 0;
    const maxVisited = 15000;
    const maxDepth = 8;
    const maxCandidates = 30;
    const maybePush = (root, absPath, candidateKind) => {
        if (candidateKind !== kind || candidates.length >= maxCandidates)
            return;
        const rel = path.relative(root, absPath).replace(/\\/g, '/');
        if (!rel || rel.startsWith('..'))
            return;
        const relLower = rel.toLowerCase();
        const nameLower = path.posix.basename(relLower);
        const matches = queryHasSlash
            ? (relLower === queryLower || relLower.endsWith('/' + queryLower))
            : nameLower === queryBase;
        if (!matches)
            return;
        candidates.push({ kind: candidateKind, path: rel, fullPath: absPath });
    };
    const scan = (root, dir, depth) => {
        if (visited >= maxVisited || candidates.length >= maxCandidates || depth > maxDepth)
            return;
        let entries = [];
        try {
            entries = fs.readdirSync(dir, { withFileTypes: true });
        }
        catch {
            return;
        }
        for (const entry of entries) {
            if (visited >= maxVisited || candidates.length >= maxCandidates)
                return;
            visited++;
            const absPath = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                maybePush(root, absPath, 'folder');
                if (!skippedDirs.has(entry.name)) {
                    scan(root, absPath, depth + 1);
                }
            }
            else if (entry.isFile()) {
                maybePush(root, absPath, 'file');
            }
        }
    };
    for (const folder of folders) {
        const root = folder.uri.fsPath;
        const direct = path.join(root, query);
        try {
            if (fs.existsSync(direct)) {
                const stat = fs.statSync(direct);
                if ((kind === 'folder' && stat.isDirectory()) || (kind === 'file' && stat.isFile())) {
                    const rel = path.relative(root, direct).replace(/\\/g, '/');
                    candidates.push({ kind, path: rel, fullPath: direct });
                }
            }
        }
        catch { }
        scan(root, root, 1);
        if (candidates.length >= maxCandidates)
            break;
    }
    const seen = new Set();
    return candidates
        .filter((item) => {
        const key = item.fullPath.toLowerCase();
        if (seen.has(key))
            return false;
        seen.add(key);
        return true;
    })
        .sort((a, b) => a.path.length - b.path.length || a.path.localeCompare(b.path))
        .slice(0, maxCandidates);
}
async function resolveDroppedPathRefs(refs, options = {}) {
    const inputRefs = Array.isArray(refs) ? refs : [];
    const resolvedRefs = [];
    for (const raw of inputRefs) {
        const item = raw && typeof raw === 'object' ? raw : {};
        const inputPath = String(item.path || '').trim();
        if (!inputPath)
            continue;
        const kind = normalizeDroppedKind(item.kind, inputPath);
        const explicitAbsolute = item.isAbsolute === true || isAbsoluteFsPath(inputPath);
        if (explicitAbsolute) {
            let existingKind = kind;
            try {
                if (fs.existsSync(inputPath)) {
                    const stat = fs.statSync(inputPath);
                    existingKind = stat.isDirectory() ? 'folder' : 'file';
                }
            }
            catch { }
            resolvedRefs.push({
                kind: existingKind,
                inputPath,
                path: inputPath,
                fullPath: inputPath,
                isAbsolute: true,
                resolved: true
            });
            continue;
        }
        const candidates = findDroppedPathCandidates(inputPath, kind);
        if (candidates.length === 1) {
            const found = candidates[0];
            resolvedRefs.push({
                kind: found.kind,
                inputPath,
                path: found.path,
                fullPath: found.fullPath,
                isAbsolute: false,
                resolved: true,
                candidates
            });
            continue;
        }
        if (candidates.length > 1 && options.promptAmbiguous !== false) {
            const picked = await vscode.window.showQuickPick(candidates.map((candidate) => ({
                label: candidate.path,
                description: candidate.kind === 'folder' ? '文件夹' : '文件',
                detail: candidate.fullPath,
                candidate
            })), {
                placeHolder: `拖入的「${inputPath}」匹配到多个位置，请选择一个`,
                matchOnDescription: true,
                matchOnDetail: true
            });
            if (picked?.candidate) {
                const found = picked.candidate;
                resolvedRefs.push({
                    kind: found.kind,
                    inputPath,
                    path: found.path,
                    fullPath: found.fullPath,
                    isAbsolute: false,
                    resolved: true,
                    candidates
                });
                continue;
            }
        }
        resolvedRefs.push({
            kind,
            inputPath,
            path: inputPath,
            isAbsolute: false,
            resolved: false,
            ambiguous: candidates.length > 1,
            candidates
        });
    }
    return {
        refs: resolvedRefs,
        resolvedCount: resolvedRefs.filter((item) => item.resolved).length,
        ambiguousCount: resolvedRefs.filter((item) => item.ambiguous).length,
        unresolvedCount: resolvedRefs.filter((item) => !item.resolved).length
    };
}
function isChannelAgentReady(channelId, now = Date.now()) {
    return isChannelOnline(channelId, now) && Boolean(readChannelWaiting(channelId, now));
}
function readAgentRecordsWithRuntimeStatus() {
    const now = Date.now();
    return readAgentRecords().map((agent) => {
        const heartbeat = readChannelHeartbeat(agent.channelId);
        const ready = isChannelAgentReady(agent.channelId, now);
        return {
            ...agent,
            status: ready ? 'online' : 'offline',
            lastSeen: heartbeat?.updatedAt || agent.lastSeen || ''
        };
    });
}
function readGroups() {
    try {
        if (!fs.existsSync(getGroupsDir())) {
            return [];
        }
        return fs.readdirSync(getGroupsDir(), { withFileTypes: true })
            .filter((entry) => entry.isDirectory())
            .map((entry) => readJsonFile(path.join(getGroupsDir(), entry.name, 'meta.json'), null))
            .filter((item) => Boolean(item && item.groupId))
            .filter((item) => item.groupId !== 'default' && item.metadata?.system !== true);
    }
    catch {
        return [];
    }
}
function writeGroup(group) {
    writeJsonFile(getGroupPath(group.groupId, 'meta.json'), group);
}
function removeLegacyDefaultTeamGroup() {
    const defaultDir = getGroupDir('default');
    try {
        if (fs.existsSync(defaultDir) && isPathInside(getGroupsDir(), defaultDir)) {
            fs.rmSync(defaultDir, { recursive: true, force: true });
        }
    }
    catch {
        // Best effort cleanup. The group is filtered from runtime snapshots even if deletion fails.
    }
}
function upgradeLegacyGroupRoleOverrides(roles) {
    const legacyValues = new Set(Object.values(LEGACY_AGENT_ROLE_DESCRIPTIONS));
    for (const group of readGroups()) {
        const metadata = group.metadata && typeof group.metadata === 'object' ? group.metadata : {};
        const memberRules = metadata.memberRuleOverrides && typeof metadata.memberRuleOverrides === 'object'
            ? metadata.memberRuleOverrides
            : {};
        const memberRoles = metadata.memberRoles && typeof metadata.memberRoles === 'object'
            ? metadata.memberRoles
            : {};
        let changed = false;
        for (const [channelId, rule] of Object.entries(memberRules)) {
            if (!legacyValues.has(String(rule || '').trim()))
                continue;
            const roleId = memberRoles[String(channelId)] || defaultRoleForChannel(String(channelId));
            const role = roles.find((item) => item.roleId === roleId);
            if (role?.description && role.description !== rule) {
                memberRules[channelId] = role.description;
                changed = true;
            }
        }
        if (changed) {
            group.metadata = { ...metadata, memberRuleOverrides: memberRules };
            group.updatedAt = nowIso();
            writeGroup(group);
        }
    }
}
function getGroupTasksPath(groupId) {
    return groupId ? getGroupPath(groupId, 'tasks.json') : getGlobalTasksPath();
}
function getGroupMemoryPath(groupId) {
    return groupId ? getGroupPath(groupId, 'memory.json') : getGlobalMemoryPath();
}
function getGroupContextsPath(groupId) {
    return groupId ? getGroupPath(groupId, 'contexts.json') : getGlobalContextsPath();
}
function getGroupEventsPath(groupId) {
    return groupId ? getGroupPath(groupId, 'events.jsonl') : getGlobalEventsPath();
}
function extractChannelIdsFromUnknown(value) {
    if (!Array.isArray(value))
        return [];
    return sortedUniqueStrings(value.map((item) => toChannelId(item, '', channelCount)).filter(Boolean));
}
function inferExpectedGroupChannelIds(group) {
    const events = readJsonLines(getGroupEventsPath(group.groupId));
    for (let i = events.length - 1; i >= 0; i--) {
        const metadata = events[i]?.metadata || {};
        const memberIds = extractChannelIdsFromUnknown(metadata.memberChannelIds);
        if (memberIds.length > 0) {
            return memberIds;
        }
    }
    const created = events.find((event) => event.type === 'group_created');
    const createdIds = extractChannelIdsFromUnknown(created?.metadata?.channelIds);
    if (createdIds.length > 0) {
        return createdIds;
    }
    return sortedUniqueStrings(group.channelIds || []);
}
function getGroupRecoveryMembers(group) {
    const roles = ensureAgentRoles();
    const agents = readAgentRecordsWithRuntimeStatus();
    const byChannel = new Map(agents.map((agent) => [String(agent.channelId), agent]));
    return sortedUniqueStrings(group.channelIds || []).map((channelId) => {
        const agent = byChannel.get(channelId);
        const roleId = getGroupAgentRoleId(group, agent);
        const role = getRoleById(roleId, roles);
        const rule = agent ? getGroupAgentRule(agent, group, roles) : '';
        return {
            channelId,
            agentId: agent?.agentId || '',
            name: agent?.name || `CH-${channelId}`,
            roleId,
            roleName: role?.name || roleId || 'Agent',
            ruleHash: stableTextHash(rule),
            skillRefs: (0, agentSkills_1.getInstalledAgentSkillRefs)(roleId),
            ready: isChannelAgentReady(channelId),
            status: agent?.status || 'offline'
        };
    });
}
function getAgentTeamGroupRecoveryStatus(groupId) {
    ensureAgentTeamRuntime();
    const group = readGroups().find((item) => item.groupId === String(groupId || ''));
    if (!group) {
        return {
            ok: false,
            groupId: String(groupId || ''),
            groupName: '',
            expectedChannelIds: [],
            currentChannelIds: [],
            readyChannelIds: [],
            missingChannelIds: [],
            extraChannelIds: [],
            notReadyChannelIds: [],
            canRestoreCurrent: false,
            message: '群组不存在',
            members: []
        };
    }
    const expectedChannelIds = inferExpectedGroupChannelIds(group);
    const currentChannelIds = sortedUniqueStrings(group.channelIds || []);
    const expectedSet = new Set(expectedChannelIds);
    const currentSet = new Set(currentChannelIds);
    const missingChannelIds = expectedChannelIds.filter((id) => !currentSet.has(id));
    const extraChannelIds = currentChannelIds.filter((id) => !expectedSet.has(id));
    const members = getGroupRecoveryMembers(group);
    const readyChannelIds = members.filter((member) => member.ready).map((member) => member.channelId);
    const readySet = new Set(readyChannelIds);
    const notReadyChannelIds = currentChannelIds.filter((id) => !readySet.has(id));
    const canRestoreCurrent = expectedChannelIds.length > 0 &&
        missingChannelIds.length === 0 &&
        extraChannelIds.length === 0 &&
        notReadyChannelIds.length === 0;
    const message = canRestoreCurrent
        ? `成员结构匹配，${readyChannelIds.length}/${currentChannelIds.length} 已接入`
        : [
            missingChannelIds.length ? `缺少历史成员：${missingChannelIds.map((id) => `CH-${id}`).join('、')}` : '',
            extraChannelIds.length ? `当前多出成员：${extraChannelIds.map((id) => `CH-${id}`).join('、')}` : '',
            notReadyChannelIds.length ? `未接入 Cursor：${notReadyChannelIds.map((id) => `CH-${id}`).join('、')}` : '',
            expectedChannelIds.length === 0 ? '没有可恢复的历史成员结构' : ''
        ].filter(Boolean).join('；');
    return {
        ok: true,
        groupId: group.groupId,
        groupName: group.name || group.groupId,
        expectedChannelIds,
        currentChannelIds,
        readyChannelIds,
        missingChannelIds,
        extraChannelIds,
        notReadyChannelIds,
        canRestoreCurrent,
        message: message || '当前群聊不可恢复',
        members
    };
}
function normalizeGroupChannelIds(channelIds, options = {}) {
    const raw = Array.isArray(channelIds) ? channelIds : [];
    const dedup = new Set();
    const now = Date.now();
    const allowChannel = (id) => !options.onlyOnline || isChannelAgentReady(id, now);
    for (const item of raw) {
        const id = toChannelId(item, '', channelCount);
        if (id && allowChannel(id)) {
            dedup.add(id);
        }
    }
    if (dedup.size === 0 && options.fallbackToAll !== false) {
        for (let i = 1; i <= channelCount; i++) {
            const id = String(i);
            if (allowChannel(id)) {
                dedup.add(id);
            }
        }
    }
    return Array.from(dedup);
}
function publishTeamEvent(input) {
    const rawMetadata = input.metadata && typeof input.metadata === 'object' ? input.metadata : {};
    const groupForMetadata = input.groupId
        ? readGroups().find((group) => group.groupId === String(input.groupId))
        : undefined;
    const memberChannelIds = groupForMetadata ? sortedUniqueStrings(groupForMetadata.channelIds || []) : [];
    const event = {
        eventId: input.eventId || makeTeamId('evt'),
        type: String(input.type || 'note'),
        title: String(input.title || ''),
        body: String(input.body || ''),
        groupId: String(input.groupId || ''),
        taskId: String(input.taskId || ''),
        metadata: memberChannelIds.length > 0 && !Array.isArray(rawMetadata.memberChannelIds)
            ? { ...rawMetadata, memberChannelIds }
            : rawMetadata,
        sourceAgentId: String(input.sourceAgentId || ''),
        sourceChannelId: String(input.sourceChannelId || ''),
        createdAt: input.createdAt || nowIso()
    };
    appendJsonLine(getGlobalEventsPath(), event);
    if (event.groupId) {
        appendJsonLine(getGroupEventsPath(event.groupId), event);
    }
    if (!event.metadata.archiveRecorded) {
        const eventChannelId = event.sourceChannelId || String(event.metadata.channelId || '');
        appendSessionArchiveMessage({
            role: event.type === 'user_message' ? 'user' : (event.type === 'agent_reply' ? 'assistant' : 'event'),
            source: event.type === 'user_message' ? 'agent_team_user' : 'agent_team_event',
            mode: 'agent-team',
            type: event.type === 'user_message' ? 'team_user_message' : `team_${event.type}`,
            title: event.title || event.type,
            content: event.body || '',
            channelId: eventChannelId,
            groupId: event.groupId || '',
            taskId: event.taskId || '',
            metadata: {
                eventId: event.eventId,
                sourceAgentId: event.sourceAgentId,
                ...event.metadata
            },
            createdAt: event.createdAt
        });
    }
    recordRecoveryEntry({
        type: `team_${event.type}`,
        scope: event.groupId ? 'group' : 'workspace',
        mode: 'agent-team',
        title: event.title || event.type,
        body: event.body || '',
        channelId: event.sourceChannelId || '',
        groupId: event.groupId || '',
        taskId: event.taskId || '',
        metadata: {
            eventId: event.eventId,
            sourceAgentId: event.sourceAgentId,
            ...event.metadata
        }
    });
    return event;
}
function resolveMentionTargets(text, group, agents, roles) {
    const body = String(text || '');
    const mentionMatches = Array.from(body.matchAll(/@([^\s@，,。:：]+)/g)).map((match) => match[1].trim());
    const groupChannelIds = new Set((group.channelIds || []).map(String));
    const now = Date.now();
    const groupAgents = agents.filter((agent) => groupChannelIds.has(String(agent.channelId)) && isChannelAgentReady(agent.channelId, now));
    if (mentionMatches.length === 0) {
        return groupAgents;
    }
    const selected = new Map();
    for (const token of mentionMatches) {
        const lower = token.toLowerCase();
        if (lower.startsWith('文件') || lower.startsWith('file') || lower.startsWith('文件夹') || lower.startsWith('folder')) {
            continue;
        }
        if (['全体', '全体成员', 'all', 'everyone'].includes(lower)) {
            for (const agent of groupAgents)
                selected.set(agent.agentId, agent);
            continue;
        }
        const chMatch = lower.match(/^ch-?(\d+)$/);
        if (chMatch) {
            const found = groupAgents.find((agent) => agent.channelId === chMatch[1]);
            if (found)
                selected.set(found.agentId, found);
            continue;
        }
        const role = roles.find((item) => item.name === token || item.roleId.toLowerCase() === lower);
        if (role) {
            for (const agent of groupAgents.filter((item) => getGroupAgentRoleId(group, item) === role.roleId)) {
                selected.set(agent.agentId, agent);
            }
            continue;
        }
        const found = groupAgents.find((agent) => agent.name === token ||
            agent.agentId.toLowerCase() === lower ||
            `ch-${agent.channelId}` === lower);
        if (found)
            selected.set(found.agentId, found);
    }
    return selected.size > 0 ? Array.from(selected.values()) : groupAgents;
}
function ensureAgentTeamRuntime() {
    if (!fs.existsSync(QUEUE_ROOT)) {
        fs.mkdirSync(QUEUE_ROOT, { recursive: true });
    }
    if (!fs.existsSync(getGroupsDir())) {
        fs.mkdirSync(getGroupsDir(), { recursive: true });
    }
    const roles = ensureAgentRoles();
    upgradeLegacyGroupRoleOverrides(roles);
    const agents = readAgentRecords();
    const byChannel = new Map(agents.map((agent) => [String(agent.channelId), agent]));
    let changed = false;
    const now = nowIso();
    for (let i = 1; i <= channelCount; i++) {
        const channelId = String(i);
        const existing = byChannel.get(channelId);
        if (!existing) {
            agents.push({
                agentId: `qingtian-ch-${channelId}`,
                channelId,
                name: `QingTian CH-${channelId}`,
                roleId: defaultRoleForChannel(channelId),
                status: 'offline',
                currentTask: '',
                workingFiles: [],
                createdAt: now,
                updatedAt: now,
                lastSeen: ''
            });
            changed = true;
            continue;
        }
        if (!existing.agentId) {
            existing.agentId = `qingtian-ch-${channelId}`;
            changed = true;
        }
        if (!existing.name) {
            existing.name = `QingTian CH-${channelId}`;
            changed = true;
        }
        if (!existing.roleId) {
            existing.roleId = defaultRoleForChannel(channelId);
            changed = true;
        }
        if (!Array.isArray(existing.workingFiles)) {
            existing.workingFiles = [];
            changed = true;
        }
    }
    if (changed) {
        writeAgentRecords(agents);
    }
    removeLegacyDefaultTeamGroup();
}
function getAgentTeamSnapshot() {
    ensureAgentTeamRuntime();
    return {
        enabled: pluginSettings.agentTeamEnabled,
        roles: ensureAgentRoles(),
        agents: readAgentRecordsWithRuntimeStatus()
    };
}
function getAgentTeamWorkbenchSnapshot(groupId) {
    ensureAgentTeamRuntime();
    const roles = ensureAgentRoles();
    const agents = readAgentRecordsWithRuntimeStatus();
    const groups = readGroups();
    const activeGroup = groups.find((group) => group.groupId === groupId) || groups[0];
    const activeGroupId = activeGroup?.groupId || '';
    const events = activeGroupId ? readJsonLines(getGroupEventsPath(activeGroupId)).slice(-200) : [];
    const tasks = activeGroupId ? readArrayFile(getGroupTasksPath(activeGroupId)).slice(0, 100) : [];
    const memories = activeGroupId ? readArrayFile(getGroupMemoryPath(activeGroupId)).slice(0, 100) : [];
    const contexts = activeGroupId ? readArrayFile(getGroupContextsPath(activeGroupId)).slice(0, 100) : [];
    return {
        enabled: pluginSettings.agentTeamEnabled,
        roles,
        agents,
        groups,
        recoveryStatus: activeGroupId ? getAgentTeamGroupRecoveryStatus(activeGroupId) : undefined,
        mentions: collectWorkspaceMentionItems(),
        activeGroupId,
        events,
        tasks,
        memories,
        contexts,
        updatedAt: Date.now()
    };
}
function createAgentTeamGroup(input) {
    ensureAgentTeamRuntime();
    if (!isAgentTeamFeatureEnabled()) {
        return { ok: false, message: 'Agent Team 协同已关闭，请在设置中开启后再创建群聊' };
    }
    const agents = readAgentRecords();
    const name = String(input.name || '').trim();
    if (!name) {
        return { ok: false, message: '请填写群名称' };
    }
    const memberInputs = normalizeMemberInputs({ channelIds: input.channelIds, members: input.members });
    const channelIds = memberInputs.map((member) => member.channelId);
    if (memberInputs.length === 0) {
        return { ok: false, message: '请先在 Cursor Agent 中接入至少一个通道，再创建群聊' };
    }
    const members = agents.filter((agent) => channelIds.includes(agent.channelId));
    const memberRoles = {};
    const memberRuleOverrides = {};
    for (const member of memberInputs) {
        if (member.roleId) {
            memberRoles[member.channelId] = member.roleId;
        }
        if (member.ruleOverride && member.ruleOverride.trim()) {
            memberRuleOverrides[member.channelId] = member.ruleOverride.trim();
        }
    }
    const now = nowIso();
    const group = {
        groupId: makeTeamId('grp'),
        name,
        goal: String(input.goal || '').trim(),
        description: String(input.description || '').trim(),
        channelIds,
        agentIds: members.map((agent) => agent.agentId),
        metadata: { memberRoles, memberRuleOverrides },
        createdAt: now,
        updatedAt: now
    };
    writeGroup(group);
    publishTeamEvent({
        type: 'group_created',
        title: `创建群组：${group.name}`,
        body: group.goal || group.description || '',
        groupId: group.groupId,
        metadata: { channelIds }
    });
    return { ok: true, group };
}
function inviteAgentsToGroup(input) {
    ensureAgentTeamRuntime();
    if (!isAgentTeamFeatureEnabled()) {
        return { ok: false, message: 'Agent Team 协同已关闭，请在设置中开启后再拉入成员' };
    }
    const groups = readGroups();
    const group = groups.find((item) => item.groupId === input.groupId);
    if (!group) {
        return { ok: false, message: '群组不存在' };
    }
    const existingIds = new Set((group.channelIds || []).map(String));
    const memberInputs = normalizeMemberInputs({ channelIds: input.channelIds, members: input.members }, { exclude: existingIds });
    if (memberInputs.length === 0) {
        return { ok: false, message: '没有可邀请的已接入 Agent' };
    }
    const agents = readAgentRecords();
    const members = agents.filter((agent) => memberInputs.some((item) => item.channelId === agent.channelId));
    for (const member of memberInputs) {
        group.channelIds.push(member.channelId);
        setGroupMemberConfig(group, member.channelId, member.roleId, member.ruleOverride);
    }
    const agentIds = new Set(group.agentIds || []);
    for (const agent of members) {
        agentIds.add(agent.agentId);
    }
    group.agentIds = Array.from(agentIds);
    group.channelIds = Array.from(new Set((group.channelIds || []).map(String)));
    group.updatedAt = nowIso();
    writeGroup(group);
    publishTeamEvent({
        type: 'members_invited',
        title: `邀请 ${memberInputs.length} 个 Agent 入群`,
        body: memberInputs.map((member) => `CH-${member.channelId}`).join('、'),
        groupId: group.groupId,
        metadata: { channelIds: memberInputs.map((member) => member.channelId) }
    });
    return { ok: true, group };
}
function deleteAgentTeamGroup(groupId) {
    ensureAgentTeamRuntime();
    const id = String(groupId || '').trim();
    if (!id) {
        return { ok: false, message: '群组不存在' };
    }
    const groups = readGroups();
    const group = groups.find((item) => item.groupId === id);
    if (!group) {
        return { ok: false, message: '群组不存在' };
    }
    const groupsDir = getGroupsDir();
    const groupDir = getGroupDir(group.groupId);
    if (!isPathInside(groupsDir, groupDir)) {
        return { ok: false, message: '群组路径异常，已取消删除' };
    }
    try {
        if (fs.existsSync(groupDir)) {
            fs.rmSync(groupDir, { recursive: true, force: true });
        }
    }
    catch (e) {
        return { ok: false, message: '删除群组失败：' + e.message };
    }
    const next = readGroups().find((item) => item.groupId !== group.groupId);
    publishTeamEvent({
        type: 'group_deleted',
        title: `删除群组：${group.name || group.groupId}`,
        body: '',
        metadata: { groupId: group.groupId, name: group.name || '' }
    });
    return { ok: true, activeGroupId: next?.groupId || '' };
}
function updateAgentRole(channelId, roleId, groupId) {
    ensureAgentTeamRuntime();
    if (!isAgentTeamFeatureEnabled()) {
        return { ok: false, message: 'Agent Team 协同已关闭，请在设置中开启后再调整角色' };
    }
    const roles = ensureAgentRoles();
    const role = roles.find((item) => item.roleId === roleId);
    if (!role) {
        return { ok: false, message: '角色不存在' };
    }
    const id = toChannelId(channelId, '', channelCount);
    if (!id) {
        return { ok: false, message: '通道不存在' };
    }
    const agents = readAgentRecords();
    const agent = agents.find((item) => item.channelId === id);
    if (!agent) {
        return { ok: false, message: 'Agent 不存在' };
    }
    if (groupId) {
        const groups = readGroups();
        const group = groups.find((item) => item.groupId === groupId);
        if (!group) {
            return { ok: false, message: '群组不存在' };
        }
        if (!group.channelIds.includes(id)) {
            return { ok: false, message: 'Agent 不在当前群组中' };
        }
        setGroupMemberConfig(group, id, role.roleId, getGroupMemberRuleOverrides(group)[id] || '');
        group.updatedAt = nowIso();
        writeGroup(group);
        publishTeamEvent({
            type: 'role_updated',
            title: `${getGroupAgentDisplayName(agent, group, roles)} 群内身份已更新`,
            body: `当前群内身份：${role.name}`,
            groupId: group.groupId,
            sourceAgentId: agent.agentId,
            sourceChannelId: agent.channelId,
            metadata: { roleId: role.roleId, scoped: 'group' }
        });
        return { ok: true, agent, group };
    }
    agent.roleId = role.roleId;
    agent.updatedAt = nowIso();
    writeAgentRecords(agents);
    publishTeamEvent({
        type: 'role_updated',
        title: `${getAgentDisplayName(agent, roles)} 角色已更新`,
        body: `当前角色：${role.name}`,
        sourceAgentId: agent.agentId,
        sourceChannelId: agent.channelId,
        metadata: { roleId: role.roleId }
    });
    return { ok: true, agent };
}
function sendAgentTeamGroupMessage(input) {
    ensureAgentTeamRuntime();
    if (!isAgentTeamFeatureEnabled()) {
        return { ok: false, message: 'Agent Team 协同已关闭，群聊消息不会投递给任何通道' };
    }
    const text = String(input.text || '').trim();
    if (!text) {
        return { ok: false, message: '消息不能为空' };
    }
    const roles = ensureAgentRoles();
    const agents = readAgentRecordsWithRuntimeStatus();
    const groups = readGroups();
    const group = groups.find((item) => item.groupId === input.groupId) || groups[0];
    if (!group) {
        return { ok: false, message: '请先创建群聊' };
    }
    const targets = resolveMentionTargets(text, group, agents, roles);
    if (targets.length === 0) {
        return { ok: false, message: '没有匹配到可投递的 Agent' };
    }
    const event = publishTeamEvent({
        type: 'user_message',
        title: input.author || '用户',
        body: text,
        groupId: group.groupId,
        metadata: {
            targetAgentIds: targets.map((agent) => agent.agentId),
            targetChannelIds: targets.map((agent) => agent.channelId)
        }
    });
    for (const agent of targets) {
        const roleId = getGroupAgentRoleId(group, agent);
        const role = getRoleById(roleId, roles);
        const rule = getGroupAgentRule(agent, group, roles);
        const installedSkills = (0, agentSkills_1.getInstalledAgentSkillRefs)(roleId);
        const prompt = [
            '[QingTian Group Chat]',
            `Group: ${group.name} (${group.groupId})`,
            `From: ${input.author || '用户'}`,
            `To: ${getGroupAgentDisplayName(agent, group, roles)}`,
            role ? `Role: ${role.name}` : '',
            rule ? `Role Rule: ${rule}` : '',
            installedSkills.length ? `Installed Skills:\n${installedSkills.map((item) => `- ${item}`).join('\n')}` : '',
            installedSkills.length ? '如果上述 Skill 与当前任务相关，优先按对应 SKILL.md 的工作流执行。' : '',
            '',
            text,
            '',
            '请在 Cursor 对话中自然回复。',
            '重要：这是 QingTian 群聊工作台消息。请优先调用 team_reply_stream 分段同步群内可见回复：先 status=start，再多次 status=delta 追加片段，最后 status=done 并传 finalText。完成后继续调用 check_messages() 保持待命即可。',
            '如果无法流式同步，下一次调用 check_messages 时必须把一条群内可见回复摘要放入 reply 参数作为兜底，例如：check_messages({ reply: "CH-' + agent.channelId + '：已收到，结论是..." })。',
            '若有进展、阻塞、决策或上下文需要同步，请同时使用 task_update、memory_write、share_context 或 publish_event 写入团队时间线。'
        ].join('\n');
        sendUserMessage(agent.channelId, { user_input: prompt });
    }
    return {
        ok: true,
        event,
        targets: targets.map((agent) => ({
            agentId: agent.agentId,
            channelId: agent.channelId,
            name: getGroupAgentDisplayName(agent, group, roles)
        }))
    };
}
function recordAgentReplyEvent(channelId, reply, timestamp) {
    const text = String(reply || '').trim();
    if (!text) {
        return;
    }
    ensureAgentTeamRuntime();
    const agents = readAgentRecords();
    const roles = ensureAgentRoles();
    const agent = agents.find((item) => item.channelId === String(channelId));
    const groups = readGroups().filter((group) => group.channelIds.includes(String(channelId)));
    recordRecoveryEntry({
        type: 'agent_reply',
        scope: groups.length > 0 ? 'group' : 'channel',
        mode: groups.length > 0 ? 'agent-team' : 'single',
        title: `${getAgentDisplayName(agent, roles)} 回复`,
        body: text,
        channelId: String(channelId),
        groupId: groups[0]?.groupId || '',
        metadata: {
            groupIds: groups.map((group) => group.groupId),
            timestamp,
            summaryOnly: true
        },
        createdAt: timestamp || nowIso()
    });
    appendSessionArchiveMessage({
        role: 'assistant',
        source: 'cursor_reply_meta',
        mode: groups.length > 0 ? 'agent-team' : 'single',
        type: 'agent_reply',
        title: `${getAgentDisplayName(agent, roles)} reply`,
        content: text,
        channelId: String(channelId),
        groupId: groups[0]?.groupId || '',
        metadata: {
            groupIds: groups.map((group) => group.groupId),
            timestamp,
            summaryOnly: true
        },
        createdAt: timestamp || nowIso()
    });
    if (!isAgentTeamFeatureEnabled()) {
        return;
    }
    const eventBase = {
        eventId: `reply_${channelId}_${Date.parse(timestamp) || Date.now()}`,
        type: 'agent_reply',
        title: `${getAgentDisplayName(agent, roles)} 回复`,
        body: text,
        sourceAgentId: agent?.agentId || '',
        sourceChannelId: String(channelId),
        createdAt: timestamp || nowIso(),
        metadata: { channelId, archiveRecorded: true }
    };
    if (groups.length === 0) {
        publishTeamEvent(eventBase);
        return;
    }
    for (const group of groups) {
        publishTeamEvent({
            ...eventBase,
            eventId: `${eventBase.eventId}_${group.groupId}`,
            title: `${getGroupAgentDisplayName(agent, group, roles)} 回复`,
            groupId: group.groupId
        });
    }
}
function getQueueDir(channelId) {
    return path.join(QUEUE_ROOT, 's', channelId);
}
function ensureQueueDir(channelId) {
    const dir = getQueueDir(channelId);
    if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true });
    }
    return dir;
}
function getHeartbeatPath(channelId) {
    return path.join(getQueueDir(channelId), 'heartbeat.json');
}
function getWaitingPath(channelId) {
    return path.join(getQueueDir(channelId), 'waiting.json');
}
function getCancelPath(channelId) {
    return path.join(getQueueDir(channelId), 'cancel.json');
}
function parseHeartbeat(raw, channelId) {
    if (!raw || typeof raw !== 'object') {
        return null;
    }
    let lastSeen = Number(raw.lastSeen);
    if (!Number.isFinite(lastSeen) && typeof raw.updatedAt === 'string') {
        lastSeen = Date.parse(raw.updatedAt);
    }
    if (!Number.isFinite(lastSeen) || lastSeen <= 0) {
        return null;
    }
    return {
        channelId,
        lastSeen,
        updatedAt: typeof raw.updatedAt === 'string' ? raw.updatedAt : undefined,
        runtimeStamp: typeof raw.runtimeStamp === 'string' && raw.runtimeStamp.trim() ? raw.runtimeStamp.trim() : undefined,
        pid: Number.isFinite(Number(raw.pid)) ? Number(raw.pid) : undefined
    };
}
function clearHeartbeat(channelId) {
    try {
        const hbPath = getHeartbeatPath(channelId);
        if (fs.existsSync(hbPath)) {
            fs.unlinkSync(hbPath);
        }
    }
    catch { }
}
function isHeartbeatProcessAlive(hb, now = Date.now()) {
    const pid = Number(hb.pid || 0);
    if (!Number.isFinite(pid) || pid <= 0)
        return null;
    if (pid === process.pid)
        return null;
    // Very old heartbeat files can point at a PID that the OS has since reused.
    // In that case, keep the timestamp stale check as the source of truth.
    if (now - hb.lastSeen > HEARTBEAT_PID_TRUST_MS)
        return null;
    try {
        process.kill(pid, 0);
        return true;
    }
    catch (e) {
        return e && e.code === 'EPERM' ? true : false;
    }
}
function isRuntimePidAlive(pid, updatedAt, now = Date.now()) {
    const normalizedPid = Number(pid || 0);
    if (!Number.isFinite(normalizedPid) || normalizedPid <= 0)
        return null;
    if (normalizedPid === process.pid)
        return null;
    if (!Number.isFinite(updatedAt) || updatedAt <= 0)
        return null;
    if (now - updatedAt > HEARTBEAT_PID_TRUST_MS)
        return null;
    try {
        process.kill(normalizedPid, 0);
        return true;
    }
    catch (e) {
        return e && e.code === 'EPERM' ? true : false;
    }
}
function clearWaiting(channelId) {
    try {
        const waitingPath = getWaitingPath(channelId);
        if (fs.existsSync(waitingPath)) {
            fs.unlinkSync(waitingPath);
        }
    }
    catch { }
}
function clearChannelCancel(channelId) {
    try {
        const cancelPath = getCancelPath(channelId);
        if (fs.existsSync(cancelPath)) {
            writeRuntimeDiagnostic('runtime_cancel_cleared', {
                title: `CH-${channelId} cancel.json cleared`,
                body: 'clearChannelCancel removed cancel.json before queueing or resetting channel state.',
                channelId,
                metadata: {
                    cancelPath,
                    reason: 'clearChannelCancel'
                }
            });
            fs.unlinkSync(cancelPath);
        }
    }
    catch { }
}
function cancelChannelWaiting(channelId, reason = 'manual_stop') {
    const id = String(channelId || '').trim();
    if (!id)
        return { ok: false, channelId: '', reason, error: 'missing_channelId' };
    try {
        const dir = ensureQueueDir(id);
        const now = Date.now();
        const normalizedReason = String(reason || 'manual_stop');
        const cancelPath = path.join(dir, 'cancel.json');
        const waitingPath = getWaitingPath(id);
        const waitingExistedBefore = fs.existsSync(waitingPath);
        fs.writeFileSync(cancelPath, JSON.stringify({
            channelId: id,
            active: true,
            reason: normalizedReason,
            updatedAt: now,
            updatedAtText: new Date(now).toISOString()
        }, null, 2), 'utf-8');
        clearWaiting(id);
        writeRuntimeDiagnostic('runtime_cancel_written', {
            title: `CH-${id} cancel.json written`,
            body: `cancelChannelWaiting wrote cancel.json with reason=${normalizedReason}`,
            channelId: id,
            metadata: {
                reason: normalizedReason,
                cancelPath,
                waitingPath,
                waitingExistedBefore,
                updatedAt: now,
                updatedAtText: new Date(now).toISOString()
            }
        });
        console.log('[QingTian][RuntimeDiag] cancelChannelWaiting wrote cancel.json', JSON.stringify({
            channelId: id,
            reason: normalizedReason,
            cancelPath,
            waitingExistedBefore,
            updatedAt: now
        }));
        return { ok: true, channelId: id, reason: normalizedReason };
    }
    catch (e) {
        const error = e instanceof Error ? e.message : String(e);
        console.error('[QingTian][RuntimeDiag] cancelChannelWaiting failed', JSON.stringify({
            channelId: id,
            reason,
            error
        }));
        return { ok: false, channelId: id, reason, error };
    }
}
/** Stop current turn on the SAME session — no new start / no new quota. */
function stopChannelTurn(channelId, options = {}) {
    // Use MAX_CHANNELS (not channelCount): stop must target the real CH-N even if
    // in-memory channelCount is stale/default (e.g. CH-5 must not clamp to CH-3).
    const id = toChannelId(channelId || '1', '1', exports.MAX_CHANNELS);
    const clearQueueFlag = options.clearQueue !== false;
    const cancel = cancelChannelWaiting(id, 'user_stop_turn');
    if (clearQueueFlag) {
        clearQueue(id);
    }
    recordChannelActivity(id);
    // Allow the next check_messages / send to proceed on the same connection.
    setTimeout(() => {
        try {
            clearChannelCancel(id);
        }
        catch { }
    }, 1500);
    return {
        ok: true,
        channelId: id,
        cancelled: cancel?.ok === true,
        queueCleared: clearQueueFlag,
        message: `已停止 CH-${id} 当前执行/等待。请继续发新消息（同一连接，无需重新开场）。`,
        guard: getChannelKeepaliveGuard(id)
    };
}
function clearChannelWaitingCancel(channelId) {
    const id = String(channelId || '').trim();
    if (!id)
        return;
    clearChannelCancel(id);
}
function recordChannelActivity(channelId, at = Date.now()) {
    const id = String(channelId || '').trim();
    if (!id) {
        return;
    }
    const safeAt = Number.isFinite(at) && at > 0 ? Math.floor(at) : Date.now();
    channelLastActivityAt[id] = Math.max(channelLastActivityAt[id] || 0, safeAt);
}
function getChannelLastActivity(channelId) {
    return channelLastActivityAt[String(channelId || '')] || 0;
}
let _preferredExtensionStorageRoot = '';
function getPreferredExtensionStorageRoot() {
    return _preferredExtensionStorageRoot || '';
}
function newestHeartbeatMtimeMs(messagesRoot) {
    try {
        const sDir = path.join(messagesRoot, 's');
        if (!fs.existsSync(sDir))
            return 0;
        let best = 0;
        for (const name of fs.readdirSync(sDir)) {
            const hb = path.join(sDir, name, 'heartbeat.json');
            if (!fs.existsSync(hb))
                continue;
            try {
                const raw = JSON.parse(fs.readFileSync(hb, 'utf-8'));
                const t = Number(raw.lastSeen || raw.updatedAt || 0);
                if (Number.isFinite(t) && t > best)
                    best = t;
            }
            catch {
                try {
                    const st = fs.statSync(hb).mtimeMs;
                    if (st > best)
                        best = st;
                }
                catch { }
            }
        }
        return best;
    }
    catch {
        return 0;
    }
}
/**
 * Brand rename left live MCP processes on QingTian.qingtian-v2 queue while
 * SlashSubs UI wrote/read SlashSubs.slashsubs queue → false disconnect after one turn.
 * Prefer whichever sibling runtime has the freshest heartbeats.
 */
function resolveCompatibleStorageRoot(preferredRoot) {
    const preferred = String(preferredRoot || '').trim() || FALLBACK_RUNTIME_ROOT;
    try {
        const parent = path.dirname(preferred);
        const base = path.basename(preferred);
        const candidates = [preferred];
        if (/slashsubs/i.test(base)) {
            candidates.push(path.join(parent, 'QingTian.qingtian-v2'));
            candidates.push(path.join(parent, 'qingtian.qingtian-v2'));
        }
        else if (/qingtian/i.test(base)) {
            candidates.push(path.join(parent, 'SlashSubs.slashsubs'));
            candidates.push(path.join(parent, 'slashsubs.slashsubs'));
        }
        let bestRoot = preferred;
        let bestTs = newestHeartbeatMtimeMs(path.join(preferred, 'runtime', 'messages'));
        for (const root of candidates) {
            if (!root || !fs.existsSync(root))
                continue;
            const ts = newestHeartbeatMtimeMs(path.join(root, 'runtime', 'messages'));
            if (ts > bestTs) {
                bestTs = ts;
                bestRoot = root;
            }
        }
        if (bestRoot !== preferred) {
            console.log('[SlashSubs] runtime path aligned to live MCP queue:', bestRoot, '(preferred was', preferred + ')');
        }
        return bestRoot;
    }
    catch (e) {
        console.warn('[SlashSubs] resolveCompatibleStorageRoot failed:', e);
        return preferred;
    }
}
function initRuntimePaths(storageRoot) {
    const preferred = storageRoot && storageRoot.trim() ? storageRoot : FALLBACK_RUNTIME_ROOT;
    _preferredExtensionStorageRoot = preferred;
    const compatible = resolveCompatibleStorageRoot(preferred);
    // Queue must match the live MCP process; deploy script stays under this extension's storage.
    RUNTIME_ROOT = path.join(compatible, 'runtime');
    QUEUE_ROOT = path.join(RUNTIME_ROOT, 'messages');
    MCP_SERVER_DEPLOY_DIR = path.join(preferred, 'runtime', 'mcp-server');
    console.log('[SlashSubs] QUEUE_ROOT=', QUEUE_ROOT);
    console.log('[SlashSubs] MCP_SERVER_DEPLOY_DIR=', MCP_SERVER_DEPLOY_DIR);
}
function getQueueRoot() {
    return QUEUE_ROOT;
}
function getRuntimeRoot() {
    return RUNTIME_ROOT;
}
function initGlobalState(state) {
    _globalState = state;
    const savedCount = _globalState?.get(STATE_KEY_CHANNEL_COUNT);
    if (typeof savedCount === 'number' && savedCount >= 1 && savedCount <= exports.MAX_CHANNELS) {
        channelCount = savedCount;
    }
    const teamEnabled = _globalState?.get(STATE_KEY_AGENT_TEAM_ENABLED);
    if (typeof teamEnabled === 'boolean') {
        pluginSettings.agentTeamEnabled = teamEnabled;
    }
    const startPromptTemplate = _globalState?.get(STATE_KEY_START_PROMPT_TEMPLATE);
    if (typeof startPromptTemplate === 'string') {
        pluginSettings.startPromptTemplate = startPromptTemplate;
    }
    ensurePluginSettingsSafe();
    ensureAgentTeamRuntime();
}
function getPluginSettings() {
    ensurePluginSettingsSafe();
    return {
        agentTeamEnabled: pluginSettings.agentTeamEnabled,
        startPromptTemplate: pluginSettings.startPromptTemplate
    };
}
function getBridgeUseProxy() {
    return _globalState?.get(STATE_KEY_BRIDGE_USE_PROXY) !== false;
}
async function setBridgeUseProxy(value) {
    const next = value !== false;
    await _globalState?.update(STATE_KEY_BRIDGE_USE_PROXY, next);
    return next;
}
function updatePluginSettings(patch) {
    const previousTeamEnabled = pluginSettings.agentTeamEnabled === true;
    if (Object.prototype.hasOwnProperty.call(patch, 'agentTeamEnabled')) {
        pluginSettings.agentTeamEnabled = Boolean(patch.agentTeamEnabled);
    }
    // 兼容旧 webview 状态包：老的“多窗口协同”开关迁移到 Agent Team 开关。
    if (Object.prototype.hasOwnProperty.call(patch, 'collaborationEnabled')) {
        pluginSettings.agentTeamEnabled = Boolean(patch.collaborationEnabled);
    }
    if (Object.prototype.hasOwnProperty.call(patch, 'startPromptTemplate')) {
        pluginSettings.startPromptTemplate = String(patch.startPromptTemplate || '').trim();
    }
    ensurePluginSettingsSafe();
    persistPluginSettings();
    ensureAgentTeamRuntime();
    const nextTeamEnabled = pluginSettings.agentTeamEnabled === true;
    if (previousTeamEnabled !== nextTeamEnabled) {
        const modeBody = nextTeamEnabled
            ? 'User enabled Agent Team. Continue from the latest group context when a group is active, while preserving previous single-channel task context.'
            : 'User disabled Agent Team. Do not route new group messages to members until Agent Team is enabled again.';
        appendSessionArchiveMessage({
            role: 'system',
            source: 'plugin_settings',
            mode: 'system',
            type: 'mode_transition',
            title: nextTeamEnabled ? 'Agent Team enabled' : 'Agent Team disabled',
            content: modeBody,
            metadata: { previousTeamEnabled, nextTeamEnabled }
        });
        recordRecoveryEntry({
            type: 'mode_transition',
            scope: 'workspace',
            mode: 'system',
            title: nextTeamEnabled ? 'Agent Team 已开启' : 'Agent Team 已关闭',
            body: nextTeamEnabled
                ? '用户从单窗口/普通通道阶段切换到多 Agent 协作可用阶段。恢复上下文时需要保留切换前的单窗口任务背景，但后续协作应按群组和角色执行。'
                : '用户关闭了多 Agent 协作。恢复上下文时不要继续向群聊成员投递消息，除非用户重新开启 Agent Team。',
            metadata: { previousTeamEnabled, nextTeamEnabled }
        });
    }
    return getPluginSettings();
}
function getChannelCount() {
    return channelCount;
}
function setChannelCount(count) {
    const nextCount = Math.floor(Number(count));
    channelCount = Number.isFinite(nextCount) ? Math.max(1, nextCount) : DEFAULT_CHANNEL_COUNT;
    _globalState?.update(STATE_KEY_CHANNEL_COUNT, channelCount);
    ensurePluginSettingsSafe();
    persistPluginSettings();
    ensureAgentTeamRuntime();
}
function tryIncrementChannelCount() {
    setChannelCount(channelCount + 1);
    return { ok: true, newCount: channelCount };
}
function writeWorkspaceInfo(workspacePath) {
    if (!fs.existsSync(QUEUE_ROOT)) {
        fs.mkdirSync(QUEUE_ROOT, { recursive: true });
    }
    fs.writeFileSync(path.join(QUEUE_ROOT, 'workspace.json'), JSON.stringify({ workspacePath, updatedAt: Date.now() }, null, 2), 'utf-8');
}
function writeRuntimeConfig(config) {
    const configPath = path.join(QUEUE_ROOT, 'config.json');
    console.log('[QingTian] 写入运行时配置到:', configPath);
    console.log('[QingTian] 输入配置:', JSON.stringify(config, null, 2));
    if (!fs.existsSync(QUEUE_ROOT)) {
        console.log('[QingTian] 创建配置目录:', QUEUE_ROOT);
        fs.mkdirSync(QUEUE_ROOT, { recursive: true });
    }
    // 始终写入所有字段，确保配置完整性
    const payload = {
        keepaliveEnabled: config.keepaliveEnabled,
        keepaliveMinutes: config.keepaliveMinutes,
        // 桥接字段：如果未提供则使用默认值
        bridgeEnabled: config.bridgeEnabled ?? false,
        bridgeChannel: config.bridgeChannel ?? 1,
        bridgeBotToken: config.bridgeBotToken ?? '',
        bridgeUseProxy: config.bridgeUseProxy !== false,
        agentTeamEnabled: config.agentTeamEnabled === true,
        updatedAt: Date.now()
    };
    console.log('[QingTian] 最终写入的配置:', JSON.stringify(payload, null, 2));
    try {
        fs.writeFileSync(configPath, JSON.stringify(payload, null, 2), 'utf-8');
        console.log('[QingTian] 配置文件写入成功');
    }
    catch (error) {
        console.error('[QingTian] 配置文件写入失败:', error);
        throw error;
    }
}
function getMimeType(ext) {
    const map = {
        '.txt': 'text/plain', '.json': 'application/json',
        '.js': 'application/javascript', '.ts': 'text/typescript',
        '.html': 'text/html', '.css': 'text/css', '.xml': 'text/xml',
        '.md': 'text/markdown', '.py': 'text/x-python',
        '.java': 'text/x-java', '.c': 'text/x-c', '.cpp': 'text/x-c++',
        '.h': 'text/x-c', '.rs': 'text/x-rust', '.go': 'text/x-go',
        '.rb': 'text/x-ruby', '.sh': 'text/x-shellscript',
        '.yaml': 'text/yaml', '.yml': 'text/yaml',
        '.toml': 'text/toml', '.csv': 'text/csv',
        '.svg': 'image/svg+xml', '.log': 'text/plain',
        '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
        '.gif': 'image/gif', '.webp': 'image/webp', '.bmp': 'image/bmp',
        '.tif': 'image/tiff', '.tiff': 'image/tiff',
        '.heic': 'image/heic', '.heif': 'image/heif', '.avif': 'image/avif'
    };
    return map[ext] || 'application/octet-stream';
}
const IMAGE_EXTS = new Set([
    '.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp', '.tif', '.tiff', '.heic', '.heif', '.avif', '.jfif'
]);
function imageExtToMime(ext) {
    if (ext === '.png')
        return 'image/png';
    if (ext === '.gif')
        return 'image/gif';
    if (ext === '.webp')
        return 'image/webp';
    if (ext === '.bmp')
        return 'image/bmp';
    if (ext === '.tif' || ext === '.tiff')
        return 'image/tiff';
    if (ext === '.heic')
        return 'image/heic';
    if (ext === '.heif')
        return 'image/heif';
    if (ext === '.avif')
        return 'image/avif';
    return 'image/jpeg';
}
/**
 * 将用户消息写入指定通道的文件队列，供 MCP Server 进程读取。
 */
function sendUserMessage(channelId, data, origin = '') {
    try {
        const dir = ensureQueueDir(channelId);
        const queuePath = path.join(dir, 'messages.json');
        let queue = { messages: [] };
        try {
            if (fs.existsSync(queuePath)) {
                queue = JSON.parse(fs.readFileSync(queuePath, 'utf-8'));
            }
        }
        catch { }
        if (!Array.isArray(queue.messages))
            queue.messages = [];
        const message = {
            text: data.user_input,
            timestamp: new Date().toISOString()
        };
        let totalBase64Chars = 0;
        const addFile = (filePath) => {
            try {
                const content = fs.readFileSync(filePath);
                const b64 = content.toString('base64');
                totalBase64Chars += b64.length;
                const name = path.basename(filePath);
                const ext = path.extname(filePath).toLowerCase();
                if (IMAGE_EXTS.has(ext)) {
                    if (!message.images)
                        message.images = [];
                    message.images.push({
                        mimeType: imageExtToMime(ext),
                        data: b64
                    });
                }
                else {
                    if (!message.files)
                        message.files = [];
                    message.files.push({
                        name,
                        mimeType: getMimeType(ext),
                        data: b64
                    });
                }
                return null;
            }
            catch (e) {
                console.error(`[QingTian] 读取附件失败: ${filePath}`, e);
                return null;
            }
        };
        if (data.file_paths) {
            for (const fp of data.file_paths) {
                const err = addFile(fp);
                if (err)
                    return { ok: false, error: err };
            }
        }
        if (data.image_paths) {
            for (const imgPath of data.image_paths) {
                try {
                    const content = fs.readFileSync(imgPath);
                    const b64 = content.toString('base64');
                    totalBase64Chars += b64.length;
                    const ext = path.extname(imgPath).toLowerCase();
                    if (!message.images)
                        message.images = [];
                    message.images.push({
                        mimeType: imageExtToMime(ext),
                        data: b64
                    });
                }
                catch (e) {
                    console.error(`[QingTian] 读取图片失败: ${imgPath}`, e);
                }
            }
        }
        clearChannelCancel(channelId);
        queue.messages.push(message);
        fs.writeFileSync(queuePath, JSON.stringify(queue, null, 2), 'utf-8');
        const recoveryMode = detectRecoveryMode(data.user_input);
        const groupId = parseGroupIdFromMessage(data.user_input || '');
        appendSessionArchiveMessage({
            role: recoveryMode === 'agent-team' ? 'event' : 'user',
            source: recoveryMode === 'agent-team' ? 'channel_dispatch' : 'plugin',
            mode: recoveryMode,
            type: recoveryMode === 'agent-team' ? 'agent_dispatch' : 'user_message',
            title: recoveryMode === 'agent-team' ? `Dispatch to CH-${channelId}` : `CH-${channelId} user message`,
            content: data.user_input || (totalBase64Chars > 0 ? '[attachments only]' : ''),
            channelId: String(channelId),
            groupId,
            attachments: normalizeArchiveAttachments(data.file_paths, data.image_paths),
            metadata: {
                fileCount: Array.isArray(data.file_paths) ? data.file_paths.length : 0,
                imageCount: Array.isArray(data.image_paths) ? data.image_paths.length : 0,
                hasEmbeddedFiles: Array.isArray(message.files) && message.files.length > 0,
                hasEmbeddedImages: Array.isArray(message.images) && message.images.length > 0
            }
        });
        recordRecoveryEntry({
            type: 'user_message',
            scope: 'channel',
            mode: recoveryMode,
            title: `CH-${channelId} 用户消息`,
            body: data.user_input || (totalBase64Chars > 0 ? '[attachments only]' : ''),
            channelId: String(channelId),
            groupId,
            metadata: {
                fileCount: Array.isArray(data.file_paths) ? data.file_paths.length : 0,
                imageCount: Array.isArray(data.image_paths) ? data.image_paths.length : 0,
                hasEmbeddedFiles: Array.isArray(message.files) && message.files.length > 0,
                hasEmbeddedImages: Array.isArray(message.images) && message.images.length > 0
            }
        });
        recordChannelActivity(channelId);
        // 跨端同步：通知 extension 把这条用户消息广播到其他端（webview / 浏览器）
        try {
            _userMessageBroadcaster?.({
                channelId: String(channelId),
                text: data.user_input || (totalBase64Chars > 0 ? '[附件]' : ''),
                timestamp: message.timestamp,
                origin: String(origin || '')
            });
        }
        catch { }
        console.log(`[QingTian] CH-${channelId} 消息已入队`);
        return { ok: true };
    }
    catch (e) {
        console.error('[QingTian] 写入消息队列失败:', e);
        return { ok: false, error: e.message };
    }
}
function enqueueRecoveryContext(channelId, input = {}) {
    try {
        const targetChannelId = String(input.targetChannelId || channelId || '1');
        const sourceChannelId = String(input.sourceChannelId || targetChannelId || '1');
        const sourceDialogId = getBoundDialogId(sourceChannelId);
        const scope = input.scope === 'workspace' || input.scope === 'group' ? input.scope : 'channel';
        const packet = getRecoveryPacket({
            scope,
            channelId: sourceChannelId,
            groupId: String(input.groupId || ''),
            depth: input.depth === 'fast' || input.depth === 'deep' ? input.depth : 'standard',
            maxChars: Number(input.maxChars || 12000)
        });
        // 原生优先：channel 作用域且调用方提供了 Cursor 原生完整对话时，用它替换恢复包正文（高保真）；
        // 否则沿用插件档案拼出的 packet.prompt（兜底）。
        const nativeText = String(input.nativeConversationText || '').trim();
        const sourceLabel = String(input.recoveryTextSource || '').trim() === 'agent-transcript'
            ? 'Cursor agent-transcripts（与 Composer @ 引用同源）'
            : String(input.recoveryTextSource || '').trim() === 'vscdb'
                ? 'Cursor state.vscdb 原生气泡'
                : 'Cursor 原生存储';
        const envelopePacket = (nativeText && scope === 'channel')
            ? { ...packet, prompt: `## 完整对话记录（来自 ${sourceLabel}）\n\n${nativeText}` }
            : packet;
        const transferredDialogId = sourceChannelId !== targetChannelId
            ? transferDialogBinding(sourceChannelId, targetChannelId, sourceDialogId)
            : (sourceDialogId || getBoundDialogId(targetChannelId));
        const dir = ensureQueueDir(targetChannelId);
        const queuePath = path.join(dir, 'messages.json');
        let queue = { messages: [] };
        try {
            if (fs.existsSync(queuePath)) {
                queue = JSON.parse(fs.readFileSync(queuePath, 'utf-8'));
            }
        }
        catch { }
        if (!Array.isArray(queue.messages))
            queue.messages = [];
        queue.messages.push({
            text: buildAutoRecoveryEnvelope(envelopePacket, sourceChannelId, targetChannelId),
            timestamp: new Date().toISOString(),
            recoveryPacket: true,
            recoveryScope: packet.scope,
            groupId: packet.groupId || '',
            recoveryDepth: packet.depth || 'standard',
            sourceChannelId,
            targetChannelId
        });
        fs.writeFileSync(queuePath, JSON.stringify(queue, null, 2), 'utf-8');
        appendSessionArchiveMessage({
            role: 'system',
            source: 'recovery_auto',
            mode: packet.groupId ? 'agent-team' : 'single',
            type: 'recovery_delivery',
            title: sourceChannelId === targetChannelId
                ? `Recovery queued for CH-${targetChannelId}`
                : `Recovery handoff CH-${sourceChannelId} -> CH-${targetChannelId}`,
            content: sourceChannelId === targetChannelId
                ? `Queued auto recovery context for CH-${targetChannelId}.`
                : `Queued recovery handoff from CH-${sourceChannelId} to CH-${targetChannelId}.`,
            channelId: targetChannelId,
            groupId: packet.groupId || '',
            metadata: {
                sourceChannelId,
                targetChannelId,
                dialogId: transferredDialogId,
                scope: packet.scope,
                depth: packet.depth || 'standard',
                entryCount: packet.entryCount,
                totalEntryCount: packet.totalEntryCount,
                auto: true
            }
        });
        recordRecoveryEntry({
            type: 'recovery_delivery',
            scope: packet.scope,
            mode: packet.groupId ? 'agent-team' : 'single',
            title: sourceChannelId === targetChannelId
                ? `Auto recovery queued for CH-${targetChannelId}`
                : `Auto recovery handoff CH-${sourceChannelId} -> CH-${targetChannelId}`,
            body: sourceChannelId === targetChannelId
                ? `Queued recovery context with ${packet.entryCount} scoped entries.`
                : `Queued recovery handoff with ${packet.entryCount} scoped entries.`,
            channelId: targetChannelId,
            groupId: packet.groupId || '',
            metadata: {
                sourceChannelId,
                targetChannelId,
                dialogId: transferredDialogId,
                depth: packet.depth || 'standard',
                totalEntryCount: packet.totalEntryCount,
                auto: true
            }
        });
        recordChannelActivity(targetChannelId);
        return { ok: true, packet };
    }
    catch (e) {
        return { ok: false, error: e.message };
    }
}
function enqueueRecoveryQueueMessage(channelId, message) {
    const dir = ensureQueueDir(channelId);
    const queuePath = path.join(dir, 'messages.json');
    let queue = { messages: [] };
    try {
        if (fs.existsSync(queuePath)) {
            queue = JSON.parse(fs.readFileSync(queuePath, 'utf-8'));
        }
    }
    catch { }
    if (!Array.isArray(queue.messages))
        queue.messages = [];
    queue.messages.push({
        timestamp: new Date().toISOString(),
        ...message
    });
    fs.writeFileSync(queuePath, JSON.stringify(queue, null, 2), 'utf-8');
    recordChannelActivity(channelId);
}
function restoreAgentTeamGroupContext(input) {
    try {
        ensureAgentTeamRuntime();
        if (!isAgentTeamFeatureEnabled()) {
            return { ok: false, message: 'Agent Team 协同已关闭，请先开启后再恢复群聊上下文' };
        }
        const group = readGroups().find((item) => item.groupId === String(input.groupId || ''));
        if (!group) {
            return { ok: false, message: '群组不存在' };
        }
        const status = getAgentTeamGroupRecoveryStatus(group.groupId);
        if (!status.canRestoreCurrent) {
            return { ok: false, groupId: group.groupId, message: status.message || '当前群聊成员结构不匹配，不能直接恢复' };
        }
        const moderatorChannelId = toChannelId(input.moderatorChannelId || status.readyChannelIds[0], '', channelCount);
        if (!moderatorChannelId || !status.readyChannelIds.includes(moderatorChannelId)) {
            return { ok: false, groupId: group.groupId, message: '请选择一个已接入 Cursor 待命的主持通道' };
        }
        const packet = getRecoveryPacket({
            scope: 'group',
            groupId: group.groupId,
            depth: input.depth === 'fast' || input.depth === 'deep' ? input.depth : 'standard',
            maxChars: Number(input.maxChars || 16000)
        });
        const targetChannelIds = status.currentChannelIds;
        for (const targetChannelId of targetChannelIds) {
            enqueueRecoveryQueueMessage(targetChannelId, {
                text: buildGroupRecoveryEnvelope({
                    packet,
                    group,
                    channelId: targetChannelId,
                    visible: targetChannelId === moderatorChannelId,
                    mode: 'group_restore'
                }),
                recoveryPacket: true,
                recoveryScope: 'group',
                groupRecovery: true,
                groupId: group.groupId,
                recoveryDepth: packet.depth || 'standard',
                moderatorChannelId,
                targetChannelId,
                silentRecovery: targetChannelId !== moderatorChannelId
            });
        }
        publishTeamEvent({
            type: 'group_recovery_queued',
            title: '群聊上下文恢复已投递',
            body: `已投递给 ${targetChannelIds.map((id) => `CH-${id}`).join('、')}，主持通道 CH-${moderatorChannelId} 负责可见确认。`,
            groupId: group.groupId,
            sourceChannelId: moderatorChannelId,
            metadata: {
                targetChannelIds,
                moderatorChannelId,
                recoveryDepth: packet.depth || 'standard',
                entryCount: packet.entryCount
            }
        });
        return {
            ok: true,
            message: `群聊上下文已投递，主持通道 CH-${moderatorChannelId} 会给出确认`,
            groupId: group.groupId,
            mode: 'group_restore',
            moderatorChannelId,
            targetChannelIds,
            packet
        };
    }
    catch (e) {
        return { ok: false, message: '恢复群聊上下文失败：' + e.message };
    }
}
function takeoverAgentTeamGroupMember(input) {
    try {
        ensureAgentTeamRuntime();
        if (!isAgentTeamFeatureEnabled()) {
            return { ok: false, message: 'Agent Team 协同已关闭，请先开启后再执行成员接管' };
        }
        const group = readGroups().find((item) => item.groupId === String(input.groupId || ''));
        if (!group) {
            return { ok: false, message: '群组不存在' };
        }
        const sourceChannelId = toChannelId(input.sourceChannelId, '', channelCount);
        const targetChannelId = toChannelId(input.targetChannelId, '', channelCount);
        if (!sourceChannelId || !targetChannelId || sourceChannelId === targetChannelId) {
            return { ok: false, groupId: group.groupId, message: '请选择有效的来源成员和目标通道' };
        }
        const currentIds = (group.channelIds || []).map(String);
        if (!currentIds.includes(sourceChannelId)) {
            return { ok: false, groupId: group.groupId, message: `CH-${sourceChannelId} 不在当前群聊中` };
        }
        if (currentIds.includes(targetChannelId)) {
            return { ok: false, groupId: group.groupId, message: `CH-${targetChannelId} 已经在当前群聊中，不能作为接管目标` };
        }
        if (!isChannelAgentReady(targetChannelId)) {
            return { ok: false, groupId: group.groupId, message: `CH-${targetChannelId} 尚未接入 Cursor 待命` };
        }
        const agents = readAgentRecords();
        const sourceAgent = agents.find((agent) => agent.channelId === sourceChannelId);
        const targetAgent = agents.find((agent) => agent.channelId === targetChannelId);
        if (!targetAgent) {
            return { ok: false, groupId: group.groupId, message: `CH-${targetChannelId} 的 Agent 记录不存在` };
        }
        const packet = getRecoveryPacket({
            scope: 'group',
            groupId: group.groupId,
            depth: input.depth === 'fast' || input.depth === 'deep' ? input.depth : 'standard',
            maxChars: Number(input.maxChars || 18000)
        });
        const roles = ensureAgentRoles();
        const sourceRoleId = getGroupAgentRoleId(group, sourceAgent);
        const sourceRule = sourceAgent ? getGroupAgentRule(sourceAgent, group, roles) : '';
        group.channelIds = currentIds.map((id) => id === sourceChannelId ? targetChannelId : id);
        group.agentIds = (group.agentIds || []).filter((id) => id !== sourceAgent?.agentId);
        if (!group.agentIds.includes(targetAgent.agentId)) {
            group.agentIds.push(targetAgent.agentId);
        }
        const metadata = getGroupMetadataObject(group);
        const memberRoles = getStringRecord(metadata.memberRoles);
        const memberRuleOverrides = getStringRecord(metadata.memberRuleOverrides);
        delete memberRoles[sourceChannelId];
        delete memberRuleOverrides[sourceChannelId];
        memberRoles[targetChannelId] = sourceRoleId;
        if (sourceRule.trim()) {
            memberRuleOverrides[targetChannelId] = sourceRule.trim();
        }
        metadata.memberRoles = memberRoles;
        metadata.memberRuleOverrides = memberRuleOverrides;
        group.channelIds = sortedUniqueStrings(group.channelIds);
        group.agentIds = Array.from(new Set((group.agentIds || []).map(String)));
        group.updatedAt = nowIso();
        const tasks = readArrayFile(getGroupTasksPath(group.groupId));
        let taskChanged = false;
        for (const task of tasks) {
            if (String(task.assigneeChannelId || '') === sourceChannelId) {
                task.assigneeChannelId = targetChannelId;
                task.assigneeAgentId = targetAgent.agentId;
                task.updatedAt = nowIso();
                taskChanged = true;
            }
        }
        if (taskChanged) {
            writeJsonFile(getGroupTasksPath(group.groupId), tasks);
        }
        writeGroup(group);
        transferDialogBinding(sourceChannelId, targetChannelId);
        enqueueRecoveryQueueMessage(targetChannelId, {
            text: buildGroupRecoveryEnvelope({
                packet,
                group,
                channelId: targetChannelId,
                visible: true,
                mode: 'member_takeover',
                sourceChannelId,
                targetChannelId
            }),
            recoveryPacket: true,
            recoveryScope: 'group',
            groupRecovery: true,
            groupId: group.groupId,
            recoveryDepth: packet.depth || 'standard',
            sourceChannelId,
            targetChannelId,
            memberTakeover: true
        });
        publishTeamEvent({
            type: 'member_takeover',
            title: `CH-${targetChannelId} 接管 CH-${sourceChannelId}`,
            body: `CH-${targetChannelId} 已继承 CH-${sourceChannelId} 的群内身份、规则、Skills 和未完成任务归属。`,
            groupId: group.groupId,
            sourceAgentId: targetAgent.agentId,
            sourceChannelId: targetChannelId,
            metadata: {
                sourceChannelId,
                targetChannelId,
                roleId: sourceRoleId,
                skillRefs: (0, agentSkills_1.getInstalledAgentSkillRefs)(sourceRoleId),
                taskReassigned: taskChanged
            }
        });
        return {
            ok: true,
            message: `CH-${targetChannelId} 已接管 CH-${sourceChannelId} 的群聊上下文`,
            groupId: group.groupId,
            mode: 'member_takeover',
            sourceChannelId,
            targetChannelId,
            targetChannelIds: [targetChannelId],
            packet
        };
    }
    catch (e) {
        return { ok: false, message: '成员接管失败：' + e.message };
    }
}

/** Quota-protection: classify whether CH should reuse current session. */
function getChannelKeepaliveGuard(channelId) {
    const id = toChannelId(channelId || '1', '1', channelCount);
    const now = Date.now();
    const hb = readChannelHeartbeat(id);
    const online = isChannelOnline(id, now);
    const waiting = readChannelWaiting(id, now);
    const waitingActive = Boolean(waiting);
    const lastAct = Math.max(
        getChannelLastActivity(id) || 0,
        hb?.lastSeen || 0,
        waiting?.updatedAt || 0
    );
    const ageMs = lastAct > 0 ? now - lastAct : null;
    let state = 'idle';
    let blockNewStart = false;
    let preferResume = false;
    let title = 'Idle — start allowed';
    let message = `CH-${id} is idle. Starting a session will consume quota.`;
    if (waitingActive || (online && waitingActive)) {
        state = 'keepalive_active';
        blockNewStart = true;
        preferResume = false;
        title = 'Keepalive active — do NOT reopen';
        message = `CH-${id} is already in check_messages standby. Do not start again (new quota). Send messages in the plugin instead.`;
    }
    else if (online || (ageMs != null && ageMs <= HEARTBEAT_STALE_MS)) {
        state = 'session_alive';
        blockNewStart = true;
        preferResume = true;
        title = 'Same session may still be alive';
        message = `CH-${id} had recent activity. Use "Resume loop" first. Forced new start consumes extra quota.`;
    }
    // Chinese titles for UI (plugin default EN labels still OK; banner uses these fields)
    if (state === 'keepalive_active') {
        title = '同一会话保活中 — 请勿重新开场';
        message = `CH-${id} 正在 check_messages 待命。请直接发消息，不要点「一键开场/复制开场」（会新开会话耗额度）。`;
    }
    else if (state === 'session_alive') {
        title = '同一会话可能仍存活 — 请先拉回循环';
        message = `CH-${id} 近期仍有活动。请点「拉回循环」在原会话继续；强制新开场会额外消耗额度。`;
    }
    else {
        title = '当前空闲 — 可以开场';
        message = `CH-${id} 当前空闲。开场会消耗一次额度，请确认后再开始。`;
    }
    return {
        channelId: id,
        state,
        blockNewStart,
        preferResume,
        online,
        waitingActive,
        lastActivityAt: lastAct || null,
        ageMs,
        title,
        message
    };
}
function getAllChannelKeepaliveGuards() {
    const out = {};
    for (let i = 1; i <= channelCount; i++) {
        out[String(i)] = getChannelKeepaliveGuard(String(i));
    }
    return out;
}
function buildResumeLoopPrompt(channelId, language = 'zh') {
    const id = toChannelId(channelId || '1', '1', channelCount);
    if (language === 'en') {
        return `[Keepalive resume / NOT a new session] Immediately call qtwx-mcp-${id} check_messages() in THIS same chat. Do not open a new Composer. Do not restart onboarding. Reuse the current connection to protect quota.`;
    }
    return `【系统保活拉回 / 非新开场】请立即在当前同一对话中调用 qtwx-mcp-${id} 的 check_messages() 继续待命。不要新开 Composer，不要重新开场，不要结束当前会话。必须复用当前连接以保护额度。`;
}
function nudgeResumeLoop(channelId) {
    const id = toChannelId(channelId || '1', '1', channelCount);
    clearChannelCancel(id);
    const prompt = buildResumeLoopPrompt(id, 'zh');
    const queued = sendUserMessage(id, { user_input: prompt }, 'keepalive-resume');
    recordChannelActivity(id);
    return {
        ok: queued?.ok !== false,
        channelId: id,
        prompt,
        queued,
        guard: getChannelKeepaliveGuard(id),
        message: queued?.ok === false
            ? (queued.error || '拉回循环失败')
            : `已向 CH-${id} 队列投递拉回指令（非新开场）。若绑定会话仍在，将尝试在原对话续跑。`
    };
}
function assertCanStartNewSession(channelId, options = {}) {
    const force = options?.force === true;
    const guard = getChannelKeepaliveGuard(channelId);
    if (!guard.blockNewStart || force) {
        return { ok: true, forced: force && guard.blockNewStart, guard };
    }
    return {
        ok: false,
        forced: false,
        guard,
        message: guard.message
    };
}

function buildDefaultStartPrompt(channelId, language = 'zh') {
    if (language === 'en') {
        return `Enter continuous dialogue mode. Repeatedly call qtwx-mcp-${channelId} check_messages to receive messages. After each complete visible reply to the user, first call record_reply({ content: your complete reply }) on the same channel to sync the full text, then continue calling check_messages until I explicitly say "end loop".`;
    }
    return `进入持续对话模式，反复调用 qtwx-mcp-${channelId} 的 check_messages 接收消息。每轮对用户的完整可见回复结束后，先调用同通道的 record_reply({ content: 你的完整回复 }) 同步全文，再继续调用 check_messages，直到我明确说「结束循环」为止。`;
}
function buildConfiguredStartPrompt(channelId, language = 'zh') {
    ensurePluginSettingsSafe();
    const custom = String(pluginSettings.startPromptTemplate || '').trim();
    const raw = custom || buildDefaultStartPrompt(channelId, language);
    const rendered = raw.replace(/\{x\}/gi, channelId).trim();
    return rendered || buildDefaultStartPrompt(channelId, language);
}
function prepareStartPrompt(channelId, language = 'zh') {
    ensureAgentTeamRuntime();
    const id = toChannelId(channelId || '1', '1', channelCount);
    clearChannelCancel(id);
    const dialog = prepareDialogStart(id);
    return {
        channelId: id,
        dialogId: dialog.dialogId,
        prompt: buildConfiguredStartPrompt(id, language)
    };
}
/**
 * 将 mcp-server/ 部署到 runtime/mcp-server/
 * 返回脚本绝对路径。
 */
function deployMCPServer(extensionPath) {
    const srcDir = path.join(extensionPath, 'mcp-server');
    if (!fs.existsSync(MCP_SERVER_DEPLOY_DIR)) {
        fs.mkdirSync(MCP_SERVER_DEPLOY_DIR, { recursive: true });
    }
    for (const file of ['index.mjs', 'package.json']) {
        const src = path.join(srcDir, file);
        const dest = path.join(MCP_SERVER_DEPLOY_DIR, file);
        if (fs.existsSync(src)) {
            fs.copyFileSync(src, dest);
        }
    }
    const srcModules = path.join(srcDir, 'node_modules');
    const destModules = path.join(MCP_SERVER_DEPLOY_DIR, 'node_modules');
    if (fs.existsSync(srcModules)) {
        copyDirSync(srcModules, destModules);
    }
    console.log(`[QingTian] MCP Server 已部署到 ${MCP_SERVER_DEPLOY_DIR}`);
    return path.join(MCP_SERVER_DEPLOY_DIR, 'index.mjs');
}
function copyDirSync(src, dest) {
    fs.mkdirSync(dest, { recursive: true });
    for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
        const s = path.join(src, entry.name);
        const d = path.join(dest, entry.name);
        entry.isDirectory() ? copyDirSync(s, d) : fs.copyFileSync(s, d);
    }
}
function getMCPServerPath() {
    return path.join(MCP_SERVER_DEPLOY_DIR, 'index.mjs');
}
function readReply(channelId) {
    try {
        const replyPath = path.join(getQueueDir(channelId), 'reply.json');
        if (!fs.existsSync(replyPath))
            return null;
        const raw = fs.readFileSync(replyPath, 'utf-8');
        const parsed = JSON.parse(raw);
        const ts = String(parsed.timestamp ?? '');
        if (!ts)
            return null;
        return { reply: String(parsed.reply ?? ''), timestamp: ts };
    }
    catch {
        return null;
    }
}
function readLatestAssistantReply(channelId) {
    try {
        const snapshotPath = getLatestReplySnapshotPath(channelId);
        if (!fs.existsSync(snapshotPath))
            return null;
        const raw = JSON.parse(fs.readFileSync(snapshotPath, 'utf-8'));
        const ts = String(raw.createdAt || '').trim();
        const text = String(raw.content || '');
        const chId = String(raw.channelId || channelId || '').trim();
        if (!ts || !text.trim() || !chId)
            return null;
        return {
            channelId: chId,
            groupId: String(raw.groupId || ''),
            messageId: String(raw.messageId || ''),
            title: String(raw.title || ''),
            content: text,
            source: String(raw.source || ''),
            createdAt: ts
        };
    }
    catch {
        return null;
    }
}
function parseReplyStreamChunk(value) {
    if (!value || typeof value !== 'object')
        return null;
    const raw = value;
    const status = String(raw.status || 'delta');
    if (!['start', 'delta', 'done', 'error'].includes(status))
        return null;
    const streamId = String(raw.streamId || '').trim();
    if (!streamId)
        return null;
    return {
        chunkId: String(raw.chunkId || `${streamId}_${raw.sequence || Date.now()}`),
        streamId,
        status: status,
        delta: String(raw.delta || ''),
        finalText: String(raw.finalText || ''),
        groupId: String(raw.groupId || ''),
        sequence: Number(raw.sequence || 0),
        channelId: String(raw.channelId || ''),
        agentId: String(raw.agentId || ''),
        timestamp: String(raw.timestamp || nowIso())
    };
}
function drainReplyStreamChunks(channelId) {
    const streamPath = path.join(getQueueDir(channelId), 'reply_stream.jsonl');
    if (!fs.existsSync(streamPath))
        return [];
    const drainPath = `${streamPath}.${process.pid}.${Date.now()}.drain`;
    try {
        fs.renameSync(streamPath, drainPath);
    }
    catch {
        return [];
    }
    try {
        const raw = fs.readFileSync(drainPath, 'utf-8');
        const chunks = raw
            .split(/\r?\n/)
            .map((line) => line.trim())
            .filter(Boolean)
            .map((line) => {
            try {
                return parseReplyStreamChunk(JSON.parse(line));
            }
            catch {
                return null;
            }
        })
            .filter((item) => Boolean(item));
        return chunks;
    }
    catch {
        return [];
    }
    finally {
        try {
            if (fs.existsSync(drainPath))
                fs.unlinkSync(drainPath);
        }
        catch { }
    }
}
function clearReply(channelId) {
    try {
        const replyPath = path.join(getQueueDir(channelId), 'reply.json');
        if (fs.existsSync(replyPath)) {
            fs.unlinkSync(replyPath);
        }
    }
    catch { }
}
function readAiDone(channelId) {
    try {
        const donePath = path.join(getQueueDir(channelId), 'ai_done.json');
        if (!fs.existsSync(donePath))
            return null;
        const raw = JSON.parse(fs.readFileSync(donePath, 'utf-8'));
        return { timestamp: Number(raw.timestamp || 0) };
    }
    catch {
        return null;
    }
}
function clearAiDone(channelId) {
    try {
        const donePath = path.join(getQueueDir(channelId), 'ai_done.json');
        if (fs.existsSync(donePath)) {
            fs.unlinkSync(donePath);
        }
    }
    catch { }
}
function readChannelHeartbeat(channelId) {
    try {
        const hbPath = getHeartbeatPath(channelId);
        if (!fs.existsSync(hbPath)) {
            return null;
        }
        const raw = JSON.parse(fs.readFileSync(hbPath, 'utf-8'));
        return parseHeartbeat(raw, channelId);
    }
    catch {
        return null;
    }
}
function readChannelWaiting(channelId, now = Date.now()) {
    try {
        const waitingPath = getWaitingPath(channelId);
        if (!fs.existsSync(waitingPath)) {
            return null;
        }
        const raw = JSON.parse(fs.readFileSync(waitingPath, 'utf-8'));
        const updatedAt = Number(raw.updatedAt || 0);
        if (!raw || raw.active !== true || !Number.isFinite(updatedAt) || updatedAt <= 0) {
            return null;
        }
        if (now - updatedAt > WAITING_STALE_MS) {
            return null;
        }
        const alive = isRuntimePidAlive(raw.pid, updatedAt, now);
        if (alive === false) {
            return null;
        }
        return {
            channelId,
            active: true,
            updatedAt,
            runtimeStamp: typeof raw.runtimeStamp === 'string' && raw.runtimeStamp.trim() ? raw.runtimeStamp.trim() : undefined,
            pid: Number.isFinite(Number(raw.pid)) ? Number(raw.pid) : undefined
        };
    }
    catch {
        return null;
    }
}
function isChannelOnline(channelId, now = Date.now()) {
    const hb = readChannelHeartbeat(channelId);
    if (!hb) {
        // Fall back to recent activity marker (covers brief gaps after check_messages returns)
        const lastAct = getChannelLastActivity(channelId);
        if (lastAct && now - lastAct <= HEARTBEAT_STALE_MS) {
            return true;
        }
        return false;
    }
    const alive = isHeartbeatProcessAlive(hb, now);
    if (alive === false) {
        return false;
    }
    if (now - hb.lastSeen <= HEARTBEAT_STALE_MS) {
        return true;
    }
    // Process still alive: keep online through long agent tool turns
    if (alive === true && now - hb.lastSeen <= HEARTBEAT_PID_TRUST_MS) {
        return true;
    }
    const lastAct = getChannelLastActivity(channelId);
    if (lastAct && now - lastAct <= HEARTBEAT_STALE_MS) {
        return true;
    }
    return false;
}
function getQueueLength(channelId) {
    try {
        const queuePath = path.join(getQueueDir(channelId), 'messages.json');
        if (!fs.existsSync(queuePath))
            return 0;
        const data = JSON.parse(fs.readFileSync(queuePath, 'utf-8'));
        return Array.isArray(data.messages) ? data.messages.length : 0;
    }
    catch {
        return 0;
    }
}
// TG bridge 独立进程直接写 messages.json（source=bridge），扩展侧轮询感知后跨端广播
const _lastBridgeMessageTs = {};
function pollBridgeUserMessages(channelCount) {
    for (let i = 1; i <= channelCount; i++) {
        const id = String(i);
        try {
            const queuePath = path.join(getQueueDir(id), 'messages.json');
            if (!fs.existsSync(queuePath))
                continue;
            const data = JSON.parse(fs.readFileSync(queuePath, 'utf-8'));
            const messages = Array.isArray(data.messages) ? data.messages : [];
            const bridgeMessages = messages.filter((m) => m && m.source === 'bridge');
            if (bridgeMessages.length === 0)
                continue;
            const maxTs = bridgeMessages.reduce((max, m) => {
                const ts = Number(m.timestamp) || 0;
                return ts > max ? ts : max;
            }, 0);
            const lastSeen = _lastBridgeMessageTs[id];
            if (lastSeen === undefined) {
                // 首次轮询：以当前队列基线为准，不回放历史 TG 消息
                _lastBridgeMessageTs[id] = maxTs;
                continue;
            }
            for (const m of bridgeMessages) {
                const ts = Number(m.timestamp) || 0;
                if (ts <= lastSeen)
                    continue;
                const text = String(m.text || '').trim();
                if (!text)
                    continue;
                try {
                    _userMessageBroadcaster?.({
                        channelId: id,
                        text,
                        timestamp: new Date(ts).toISOString(),
                        origin: 'bridge'
                    });
                }
                catch { }
                if (ts > (_lastBridgeMessageTs[id] || 0)) {
                    _lastBridgeMessageTs[id] = ts;
                }
            }
        }
        catch { }
    }
}
function clearQueue(channelId) {
    try {
        const queuePath = path.join(getQueueDir(channelId), 'messages.json');
        if (fs.existsSync(queuePath)) {
            fs.writeFileSync(queuePath, JSON.stringify({ messages: [] }, null, 2), 'utf-8');
        }
    }
    catch { }
}
function tryDecrementChannelCount() {
    if (channelCount <= 1) {
        return { ok: false, message: '至少保留 1 个通道' };
    }
    const removed = channelCount;
    clearQueue(String(removed));
    clearReply(String(removed));
    clearHeartbeat(String(removed));
    clearWaiting(String(removed));
    if (channelLastActivityAt[String(removed)]) {
        delete channelLastActivityAt[String(removed)];
    }
    setChannelCount(channelCount - 1);
    return { ok: true, removedCount: removed };
}
function getMCPStatus() {
    const now = Date.now();
    const channelStatuses = {};
    for (let i = 1; i <= channelCount; i++) {
        const id = String(i);
        const hb = readChannelHeartbeat(id);
        const waiting = readChannelWaiting(id, now);
        // Refresh activity from live runtime files so grace window stays warm
        if (hb?.lastSeen) {
            recordChannelActivity(id, hb.lastSeen);
        }
        if (waiting?.updatedAt) {
            recordChannelActivity(id, waiting.updatedAt);
        }
        const online = isChannelOnline(id, now);
        channelStatuses[id] = {
            waiting: false,
            callCount: 0,
            queueLength: getQueueLength(id),
            online,
            lastSeen: hb ? hb.lastSeen : null,
            waitingActive: Boolean(waiting && online),
            waitingUpdatedAt: waiting ? waiting.updatedAt : null
        };
    }
    const keepaliveGuards = getAllChannelKeepaliveGuards();
    return {
        mcpConnected: fs.existsSync(getMCPServerPath()),
        hasActiveSession: Object.values(keepaliveGuards).some((g) => g.state !== 'idle'),
        waiting: Object.values(keepaliveGuards).some((g) => g.waitingActive),
        callCount: 0,
        channelCount,
        channels: channelStatuses,
        keepaliveGuards
    };
}
//# sourceMappingURL=mcpServer.js.map