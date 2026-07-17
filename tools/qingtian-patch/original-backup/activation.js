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
exports.initEncryptionKey = initEncryptionKey;
exports.getRuntimePolicy = getRuntimePolicy;
exports.renderPolicyTemplate = renderPolicyTemplate;
exports.clearLicense = clearLicense;
exports.setActivationInvalidHandler = setActivationInvalidHandler;
exports.setServerConnectionHandler = setServerConnectionHandler;
exports.setUpdatePushHandler = setUpdatePushHandler;
exports.isServerDisconnected = isServerDisconnected;
exports.startUpdatePush = startUpdatePush;
exports.stopUpdatePush = stopUpdatePush;
exports.isActivated = isActivated;
exports.checkActivation = checkActivation;
exports.activate = activate;
exports.getLicenseInfo = getLicenseInfo;
exports.getExpiresText = getExpiresText;
exports.getLicenseCountdownStatus = getLicenseCountdownStatus;
exports.stopPeriodicVerify = stopPeriodicVerify;
const vscode = __importStar(require("vscode"));
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
const os = __importStar(require("os"));
const http = __importStar(require("http"));
const https = __importStar(require("https"));
const net = __importStar(require("net"));
const tls = __importStar(require("tls"));
const crypto = __importStar(require("crypto"));
const OFFICIAL_LICENSE_API_BASE = 'https://qingt.top/prod-api';
const TRUSTED_LICENSE_HOSTS = new Set([
    'qingt.top',
    'www.qingt.top',
    '111.228.36.85',
    'localhost',
    '127.0.0.1'
]);
function getLicenseApiBase() {
    try {
        const cfg = vscode.workspace.getConfiguration('qingtian');
        const base = (cfg.get('licenseApiBase') || '').trim();
        if (base) {
            const normalized = base.replace(/\/$/, '');
            if (validateEndpoint(normalized)) {
                return normalized;
            }
            void reportSecurityEvent('untrusted_api_base', {
                configuredApiBase: normalized
            });
            console.warn('[QingTian] Untrusted licenseApiBase ignored:', normalized);
        }
    }
    catch (_) { }
    return OFFICIAL_LICENSE_API_BASE;
}
function validateEndpoint(url) {
    try {
        const parsed = new URL(url);
        if (!TRUSTED_LICENSE_HOSTS.has(parsed.hostname)) {
            return false;
        }
        if (parsed.hostname === 'localhost' || parsed.hostname === '127.0.0.1') {
            return parsed.protocol === 'http:' || parsed.protocol === 'https:';
        }
        return parsed.protocol === 'https:';
    }
    catch (_) {
        return false;
    }
}
// ===== 服务端加密密钥管理 =====
let _serverEncryptionKey = null;
/**
 * 启动时从服务端获取加密密钥
 */
async function initEncryptionKey() {
    try {
        const result = await httpPost('/api/qt/init', { version: getVersion() });
        if (result.code === 200 && result.encryptionKey) {
            _serverEncryptionKey = result.encryptionKey;
            console.log('[QingTian] 已从服务器获取加密密钥');
            return true;
        }
        else {
            console.error('[QingTian] 服务器未返回加密密钥');
            return false;
        }
    }
    catch (e) {
        console.error('[QingTian] 获取加密密钥失败:', e);
        return false;
    }
}
function isServerKeyReady() {
    return _serverEncryptionKey !== null;
}
const DEFAULT_RUNTIME_POLICY = {
    version: 'local-default',
    toolDescription: 'QingTian MCP bridge',
    startPromptTemplate: `你好，本次对话使用 {{MCP_NAME}} 工具进行交互。

【最高优先级规则 - 本次及后续所有对话必须严格遵守】

【强制规则】每次回复结束前，必须调用 {{MCP_NAME}} 工具等待用户的下次指令。

**调用参数：**
- title: 简短描述当前完成的任务
- summary: 详细说明完成了什么工作

**【可选】猜测用户下一步操作：**
在 summary 末尾添加选项：
\`\`\`
摘要内容...

---choices---
猜测选项1
猜测选项2
\`\`\`

【严禁】
- 禁止完成任务后不调用 {{MCP_NAME}} 工具
- 禁止询问"还需要什么帮助"后不调用MCP
- 禁止调用MCP工具后继续输出内容
- 不调用MCP工具就结束回复是违规行为！

---
以上规则在整个对话过程中持续有效！`,
    windsurfRulesTemplate: `# 晴天无限MCP - AI对话持续规则

【最高优先级规则 - 本次及后续所有对话必须严格遵守】

【强制规则】每次回复结束前，必须调用 {{MCP_NAME}} 工具等待用户的下次指令。

**调用参数：**
- title: 简短描述当前完成的任务
- summary: 详细说明完成了什么工作

**【可选】猜测用户下一步操作：**
在 summary 末尾添加选项：
\`\`\`
摘要内容...

---choices---
猜测选项1
猜测选项2
\`\`\`

【严禁】
- 禁止完成任务后不调用 {{MCP_NAME}} 工具
- 禁止询问"还需要什么帮助"后不调用MCP
- 禁止调用MCP工具后继续输出内容
- 不调用MCP工具就结束回复是违规行为！

---
以上规则在整个对话过程中持续有效！`
};
const OFFLINE_LICENSE_PUBLIC_KEY_PEM = `-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA9UaV3GpI8jl6ZCs6Coac
hVnE+v1nVVhSUB7FAhSlVsxld92xjuZ/AizRoHCcvTEPQm9xgjRzXs5P+3UyqwYA
JuaoY8dHy8pj3MP2jVHEta1EYDVYNGB3YPJMV2EjD0xAWwAfKKveJ/Pn+zHX3+0e
i5ZipaIYtvL2JCISo/FK1GDzsmpA2YxfOPF/IfC1av4aEKYQVQY4GGF9GCb8aWAU
A2udJlSrOxTUECbIyWEpT5FS4lQXVtxOwHdmXnbihtX6D/nKWE9ATptoalmUPSP2
OXJ2Op+Av22FtzQAF1A2Uedk2oO+y1nnJbLwilTTjXjvSlJ748xcw9ByrMYIWLQs
twIDAQAB
-----END PUBLIC KEY-----`;
const LICENSE_DIR = path.join(os.homedir(), '.qingtian-mcp');
const LICENSE_FILE = path.join(LICENSE_DIR, 'license.dat');
function getLocalEncryptKey(machineId = getMachineId()) {
    return crypto.createHash('sha256').update('qt_' + machineId + '_license').digest();
}
function getLegacyServerEncryptKey() {
    if (!_serverEncryptionKey) {
        return null;
    }
    return crypto.createHash('sha256').update(_serverEncryptionKey).digest();
}
function encryptWithKey(text, key) {
    const iv = crypto.randomBytes(16);
    const cipher = crypto.createCipheriv('aes-256-cbc', key, iv);
    let encrypted = cipher.update(text, 'utf8', 'hex');
    encrypted += cipher.final('hex');
    return iv.toString('hex') + ':' + encrypted;
}
function encrypt(text) {
    return encryptWithKey(text, getLocalEncryptKey());
}
function decryptWithKey(data, key) {
    const parts = data.split(':');
    if (parts.length !== 2) {
        throw new Error('加密数据格式错误');
    }
    const iv = Buffer.from(parts[0], 'hex');
    const decipher = crypto.createDecipheriv('aes-256-cbc', key, iv);
    let decrypted = decipher.update(parts[1], 'hex', 'utf8');
    decrypted += decipher.final('utf8');
    return decrypted;
}
function decrypt(data) {
    return decryptWithKey(data, getLocalEncryptKey());
}
function normalizePolicy(raw) {
    if (!raw || typeof raw !== 'object') {
        return undefined;
    }
    return {
        version: String(raw.version || DEFAULT_RUNTIME_POLICY.version),
        toolDescription: String(raw.toolDescription || DEFAULT_RUNTIME_POLICY.toolDescription),
        startPromptTemplate: String(raw.startPromptTemplate || DEFAULT_RUNTIME_POLICY.startPromptTemplate),
        windsurfRulesTemplate: String(raw.windsurfRulesTemplate || DEFAULT_RUNTIME_POLICY.windsurfRulesTemplate)
    };
}
function getRuntimePolicy() {
    const policy = loadLicense()?.policy;
    return {
        ...DEFAULT_RUNTIME_POLICY,
        ...(policy || {})
    };
}
function renderPolicyTemplate(template, vars) {
    return Object.entries(vars).reduce((text, [key, value]) => {
        const token = `{{${key}}}`;
        return text.split(token).join(String(value));
    }, template);
}
// ===== 持久化设备ID多点存储 =====
const DEVICE_ID_FILE = path.join(os.homedir(), '.qt_device_id');
const DEVICE_ID_REG_KEY = 'HKCU\\Software\\QingTianMcp';
const DEVICE_ID_REG_VALUE = 'DeviceId';
function readDeviceIdFromFile() {
    try {
        if (fs.existsSync(DEVICE_ID_FILE)) {
            return fs.readFileSync(DEVICE_ID_FILE, 'utf8').trim();
        }
    }
    catch (_) { }
    return null;
}
function writeDeviceIdToFile(id) {
    try {
        fs.writeFileSync(DEVICE_ID_FILE, id, 'utf8');
        // 隐藏文件 (Windows)
        if (process.platform === 'win32') {
            require('child_process').execSync(`attrib +h "${DEVICE_ID_FILE}"`, { stdio: 'ignore' });
        }
    }
    catch (_) { }
}
function readDeviceIdFromRegistry() {
    if (process.platform !== 'win32')
        return null;
    try {
        const { execSync } = require('child_process');
        const output = execSync(`reg query "${DEVICE_ID_REG_KEY}" /v ${DEVICE_ID_REG_VALUE}`, { encoding: 'utf8', stdio: ['pipe', 'pipe', 'ignore'] });
        const match = output.match(/REG_SZ\s+(.+)/);
        return match ? match[1].trim() : null;
    }
    catch (_) { }
    return null;
}
function writeDeviceIdToRegistry(id) {
    if (process.platform !== 'win32')
        return;
    try {
        const { execSync } = require('child_process');
        execSync(`reg add "${DEVICE_ID_REG_KEY}" /v ${DEVICE_ID_REG_VALUE} /t REG_SZ /d "${id}" /f`, { stdio: 'ignore' });
    }
    catch (_) { }
}
function getMachineId() {
    const vsId = vscode.env.machineId;
    const fileId = readDeviceIdFromFile();
    const regId = readDeviceIdFromRegistry();
    // 优先使用已持久化的ID（防止machineId被重置绕过一码一机）
    const persistedId = fileId || regId;
    if (persistedId) {
        // 确保所有存储点一致
        if (!fileId)
            writeDeviceIdToFile(persistedId);
        if (!regId)
            writeDeviceIdToRegistry(persistedId);
        return persistedId;
    }
    // 全新安装：使用vscode.env.machineId并写入所有存储点
    writeDeviceIdToFile(vsId);
    writeDeviceIdToRegistry(vsId);
    return vsId;
}
// ===== 服务器时间校正 =====
function calculateServerTime(license) {
    if (license.serverTime && license.lastSyncTime) {
        const elapsed = Date.now() - license.lastSyncTime;
        if (elapsed < 0) {
            // 检测到本地时间回拨，使用服务器时间
            console.warn('[QingTian] 检测到本地时间回拨，使用服务器时间');
            return license.serverTime;
        }
        return license.serverTime + elapsed;
    }
    return Date.now();
}
// 保存激活信息（加密）
function writeFileAtomically(targetFile, content) {
    const tempFile = `${targetFile}.${process.pid}.${Date.now()}.tmp`;
    fs.writeFileSync(tempFile, content, 'utf8');
    try {
        fs.renameSync(tempFile, targetFile);
    }
    catch (e) {
        try {
            fs.unlinkSync(tempFile);
        }
        catch (_) { }
        throw e;
    }
}
function saveLicense(info) {
    try {
        if (!fs.existsSync(LICENSE_DIR)) {
            fs.mkdirSync(LICENSE_DIR, { recursive: true });
        }
        const json = JSON.stringify(info);
        const encrypted = encryptWithKey(json, getLocalEncryptKey(info.machineId));
        // 多窗口会共享同一份授权文件，原子替换可以避免读到半截内容。
        writeFileAtomically(LICENSE_FILE, encrypted);
    }
    catch (e) {
        console.error('[QingTian] 保存激活信息失败:', e);
    }
}
function normalizeLicenseInfo(raw, machineId) {
    return {
        code: raw.code,
        refreshToken: raw.refreshToken,
        ticket: raw.ticket,
        ticketExpiresAt: raw.ticketExpiresAt ? Number(raw.ticketExpiresAt) : undefined,
        offlineLicense: raw.offlineLicense ? String(raw.offlineLicense) : undefined,
        machineId: raw.machineId || machineId,
        activatedAt: Number(raw.activatedAt || Date.now()),
        expiresAt: raw.expiresAt ? Number(raw.expiresAt) : null,
        durationType: Number(raw.durationType || 0),
        lastVerified: Number(raw.lastVerified || 0),
        serverTime: raw.serverTime ? Number(raw.serverTime) : undefined,
        lastSyncTime: raw.lastSyncTime ? Number(raw.lastSyncTime) : undefined,
        policy: normalizePolicy(raw.policy),
        packageWatermarkId: raw.packageWatermarkId ? String(raw.packageWatermarkId) : undefined,
        packageCodeHash: raw.packageCodeHash ? String(raw.packageCodeHash) : undefined,
        packageVersion: raw.packageVersion ? String(raw.packageVersion) : undefined,
        packageGeneratedAt: raw.packageGeneratedAt ? String(raw.packageGeneratedAt) : undefined,
        packageSignature: raw.packageSignature ? String(raw.packageSignature) : undefined,
        packageBoundAt: raw.packageBoundAt ? Number(raw.packageBoundAt) : undefined
    };
}
function decodeBase64Url(input) {
    const normalized = input.replace(/-/g, '+').replace(/_/g, '/');
    const padding = normalized.length % 4;
    const padded = padding === 0 ? normalized : normalized + '='.repeat(4 - padding);
    return Buffer.from(padded, 'base64');
}
function parseOfflineLicense(token) {
    try {
        const parts = token.split('.');
        if (parts.length !== 2) {
            return null;
        }
        const [encodedPayload, encodedSignature] = parts;
        const verifier = crypto.createVerify('RSA-SHA256');
        verifier.update(encodedPayload, 'utf8');
        verifier.end();
        const valid = verifier.verify(OFFLINE_LICENSE_PUBLIC_KEY_PEM, decodeBase64Url(encodedSignature));
        if (!valid) {
            return null;
        }
        const raw = JSON.parse(decodeBase64Url(encodedPayload).toString('utf8'));
        if (!raw || raw.type !== 'offline-license') {
            return null;
        }
        return {
            type: String(raw.type),
            ver: raw.ver ? Number(raw.ver) : undefined,
            code: String(raw.code || ''),
            machineId: String(raw.machineId || ''),
            activatedAt: raw.activatedAt != null ? Number(raw.activatedAt) : undefined,
            expiresAt: raw.expiresAt != null ? Number(raw.expiresAt) : null,
            durationType: raw.durationType != null ? Number(raw.durationType) : undefined,
            iat: raw.iat != null ? Number(raw.iat) : undefined
        };
    }
    catch (e) {
        console.warn('[QingTian] 解析离线许可证失败:', e);
        return null;
    }
}
function validateOfflineLicense(license, machineId) {
    if (!license.offlineLicense) {
        return { ok: false, message: 'missing offline license' };
    }
    const payload = parseOfflineLicense(license.offlineLicense);
    if (!payload) {
        return { ok: false, message: '离线许可证签名无效' };
    }
    if (!payload.code || !payload.machineId) {
        return { ok: false, message: '离线许可证内容不完整' };
    }
    if (payload.machineId !== machineId) {
        return { ok: false, message: '离线许可证与当前设备不匹配' };
    }
    if (payload.expiresAt && payload.expiresAt < Date.now()) {
        return { ok: false, message: '激活码已过期' };
    }
    return { ok: true, payload };
}
function applyOfflineLicensePayload(license, payload) {
    return {
        ...license,
        code: payload.code || license.code,
        machineId: payload.machineId || license.machineId,
        activatedAt: payload.activatedAt != null ? payload.activatedAt : license.activatedAt,
        expiresAt: payload.expiresAt != null ? payload.expiresAt : null,
        durationType: payload.durationType != null ? payload.durationType : license.durationType
    };
}
// 读取激活信息（解密，兼容新旧密钥）
function loadLicense() {
    try {
        if (!fs.existsSync(LICENSE_FILE)) {
            return null;
        }
        const encrypted = fs.readFileSync(LICENSE_FILE, 'utf8');
        const machineId = getMachineId();
        let json;
        let migrated = false;
        try {
            json = decryptWithKey(encrypted, getLocalEncryptKey(machineId));
        }
        catch {
            const legacyServerKey = getLegacyServerEncryptKey();
            try {
                if (!legacyServerKey) {
                    throw new Error('legacy key missing');
                }
                json = decryptWithKey(encrypted, legacyServerKey);
                migrated = true;
                console.log('[QingTian] 使用旧服务端密钥解密成功，将迁移到本地密钥');
            }
            catch {
                console.error('[QingTian] 解密失败，激活数据可能已被篡改');
                if (isServerKeyReady()) {
                    clearLicense();
                    vscode.window.showErrorMessage('⚠️ 检测到激活数据异常！数据可能已被篡改或损坏，已清除本地数据。请重新输入激活码。', '重新激活').then(sel => {
                        if (sel === '重新激活') {
                            vscode.commands.executeCommand('qingtian.openPanel');
                        }
                    });
                }
                return null;
            }
        }
        const info = normalizeLicenseInfo(JSON.parse(json), machineId);
        if (info.machineId !== machineId) {
            console.log('[QingTian] 机器指纹不匹配，激活无效');
            return null;
        }
        if (migrated) {
            saveLicense(info);
        }
        return info;
    }
    catch (e) {
        console.error('[QingTian] 读取激活信息失败:', e);
        return null;
    }
}
function clearLicense() {
    try {
        if (fs.existsSync(LICENSE_FILE)) {
            fs.unlinkSync(LICENSE_FILE);
        }
    }
    catch (e) {
        console.error('[QingTian] 清除激活信息失败:', e);
    }
}
// ===== HTTP请求 =====
function buildApiUrl(urlPath, baseOverride) {
    const base = (baseOverride || getLicenseApiBase()).replace(/\/$/, '') + '/';
    const rel = urlPath.startsWith('/') ? urlPath.slice(1) : urlPath;
    return new URL(rel, base);
}
function normalizeSingleProxyUrl(raw) {
    const value = (raw || '').trim();
    if (!value) {
        return '';
    }
    const withProtocol = /^[a-z][a-z0-9+.-]*:\/\//i.test(value) ? value : `http://${value}`;
    try {
        const parsed = new URL(withProtocol);
        if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
            return '';
        }
        return parsed.toString();
    }
    catch {
        return '';
    }
}
function normalizeProxyUrl(raw) {
    const value = (raw || '').trim();
    if (!value) {
        return '';
    }
    if (value.includes(';')) {
        const parts = {};
        for (const part of value.split(';')) {
            const match = part.trim().match(/^([^=]+)=(.+)$/);
            if (match) {
                parts[match[1].trim().toLowerCase()] = match[2].trim();
            }
        }
        return normalizeSingleProxyUrl(parts.https || parts.http || '');
    }
    return normalizeSingleProxyUrl(value);
}
function readWindowsSystemProxy() {
    if (process.platform !== 'win32') {
        return '';
    }
    try {
        const { execSync } = require('child_process');
        const enableOut = execSync('reg query "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings" /v ProxyEnable', { encoding: 'utf8', timeout: 3000, stdio: ['pipe', 'pipe', 'pipe'] });
        const enableMatch = enableOut.match(/ProxyEnable\s+REG_DWORD\s+0x([0-9a-f]+)/i);
        if (!enableMatch || parseInt(enableMatch[1], 16) !== 1) {
            return '';
        }
        const regOut = execSync('reg query "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings" /v ProxyServer', { encoding: 'utf8', timeout: 3000, stdio: ['pipe', 'pipe', 'pipe'] });
        const match = regOut.match(/ProxyServer\s+REG_SZ\s+(.+)/);
        return normalizeProxyUrl(match ? match[1].trim() : '');
    }
    catch {
        return '';
    }
}
function readMacSystemProxy() {
    if (process.platform !== 'darwin') {
        return '';
    }
    try {
        const { execSync } = require('child_process');
        for (const service of ['Wi-Fi', 'Ethernet']) {
            try {
                const out = execSync(`networksetup -getwebproxy ${JSON.stringify(service)}`, {
                    encoding: 'utf8',
                    timeout: 3000,
                    stdio: ['pipe', 'pipe', 'pipe']
                });
                const enabledMatch = out.match(/Enabled:\s*(Yes|No)/i);
                const serverMatch = out.match(/Server:\s*(.+)/);
                const portMatch = out.match(/Port:\s*(\d+)/);
                if (enabledMatch?.[1].toLowerCase() === 'yes' && serverMatch && portMatch) {
                    const proxy = normalizeProxyUrl(`${serverMatch[1].trim()}:${portMatch[1].trim()}`);
                    if (proxy) {
                        return proxy;
                    }
                }
            }
            catch { }
        }
    }
    catch { }
    return '';
}
function shouldBypassProxy(hostname) {
    const host = hostname.toLowerCase();
    if (host === 'localhost' || host === '127.0.0.1' || host === '::1') {
        return true;
    }
    const noProxy = process.env.NO_PROXY || process.env.no_proxy || '';
    return noProxy.split(',').some((entry) => {
        const rule = entry.trim().toLowerCase();
        if (!rule) {
            return false;
        }
        if (rule === '*') {
            return true;
        }
        if (rule.startsWith('.')) {
            return host.endsWith(rule);
        }
        return host === rule || host.endsWith(`.${rule}`);
    });
}
function getActivationProxy(targetUrl) {
    if (shouldBypassProxy(targetUrl.hostname)) {
        return null;
    }
    const httpProxySetting = vscode.workspace.getConfiguration('http').get('proxy', '');
    const candidates = [
        httpProxySetting,
        process.env.HTTPS_PROXY,
        process.env.HTTP_PROXY,
        process.env.https_proxy,
        process.env.http_proxy,
        process.env.ALL_PROXY,
        process.env.all_proxy,
        readWindowsSystemProxy(),
        readMacSystemProxy()
    ];
    for (const candidate of candidates) {
        const normalized = normalizeProxyUrl(candidate || '');
        if (normalized) {
            return new URL(normalized);
        }
    }
    return null;
}
function getProxyAuthorization(proxyUrl) {
    if (!proxyUrl.username) {
        return undefined;
    }
    const username = decodeURIComponent(proxyUrl.username);
    const password = decodeURIComponent(proxyUrl.password || '');
    return `Basic ${Buffer.from(`${username}:${password}`).toString('base64')}`;
}
function connectSocketToProxy(proxyUrl) {
    return new Promise((resolve, reject) => {
        const port = proxyUrl.port ? parseInt(proxyUrl.port, 10) : (proxyUrl.protocol === 'https:' ? 443 : 80);
        const onConnect = () => {
            socket.setTimeout(0);
            socket.removeListener('error', reject);
            resolve(socket);
        };
        const socket = proxyUrl.protocol === 'https:'
            ? tls.connect({ host: proxyUrl.hostname, port, servername: proxyUrl.hostname }, onConnect)
            : net.connect({ host: proxyUrl.hostname, port }, onConnect);
        socket.setTimeout(10000, () => {
            socket.destroy(new Error('Proxy connection timeout'));
        });
        socket.once('error', reject);
    });
}
function createProxyTunnel(targetUrl, proxyUrl) {
    return new Promise(async (resolve, reject) => {
        let proxySocket = null;
        try {
            proxySocket = await connectSocketToProxy(proxyUrl);
            const targetPort = targetUrl.port ? parseInt(targetUrl.port, 10) : (targetUrl.protocol === 'https:' ? 443 : 80);
            const authHeader = getProxyAuthorization(proxyUrl);
            const headers = [
                `CONNECT ${targetUrl.hostname}:${targetPort} HTTP/1.1`,
                `Host: ${targetUrl.hostname}:${targetPort}`,
                'Proxy-Connection: Keep-Alive'
            ];
            if (authHeader) {
                headers.push(`Proxy-Authorization: ${authHeader}`);
            }
            proxySocket.write(headers.join('\r\n') + '\r\n\r\n');
            let buffer = Buffer.alloc(0);
            const fail = (err) => {
                proxySocket?.destroy();
                reject(err);
            };
            proxySocket.setTimeout(10000, () => fail(new Error('Proxy tunnel timeout')));
            proxySocket.on('error', fail);
            proxySocket.on('data', (chunk) => {
                buffer = Buffer.concat([buffer, chunk]);
                const headerEnd = buffer.indexOf('\r\n\r\n');
                if (headerEnd === -1) {
                    return;
                }
                const headerText = buffer.slice(0, headerEnd).toString('latin1');
                const statusMatch = headerText.match(/^HTTP\/\d\.\d\s+(\d+)/i);
                const status = statusMatch ? parseInt(statusMatch[1], 10) : 0;
                if (status < 200 || status >= 300) {
                    fail(new Error(`Proxy CONNECT failed: HTTP ${status || 'unknown'}`));
                    return;
                }
                proxySocket?.removeAllListeners('data');
                proxySocket?.removeAllListeners('error');
                proxySocket?.setTimeout(0);
                if (targetUrl.protocol === 'https:') {
                    const tlsSocket = tls.connect({ socket: proxySocket, servername: targetUrl.hostname }, () => {
                        resolve(tlsSocket);
                    });
                    tlsSocket.once('error', reject);
                }
                else {
                    resolve(proxySocket);
                }
            });
        }
        catch (e) {
            proxySocket?.destroy();
            reject(e);
        }
    });
}
function createProxyAgent(targetUrl, proxyUrl) {
    const agent = new https.Agent({ keepAlive: false });
    agent.createConnection =
        (_options, callback) => {
            if (!callback) {
                return undefined;
            }
            createProxyTunnel(targetUrl, proxyUrl)
                .then((socket) => callback(null, socket))
                .catch((err) => callback(err instanceof Error ? err : new Error(String(err))));
            return undefined;
        };
    return agent;
}
function buildRequestOptions(fullUrl, method, headers) {
    const proxyUrl = getActivationProxy(fullUrl);
    const targetPort = fullUrl.port ? parseInt(fullUrl.port, 10) : (fullUrl.protocol === 'https:' ? 443 : 80);
    if (proxyUrl && fullUrl.protocol === 'https:') {
        console.log('[QingTian] License request using proxy:', `${proxyUrl.protocol}//${proxyUrl.host}`);
        return {
            client: https,
            options: {
                hostname: fullUrl.hostname,
                port: targetPort,
                path: fullUrl.pathname + fullUrl.search,
                method,
                headers,
                timeout: 10000,
                agent: createProxyAgent(fullUrl, proxyUrl)
            }
        };
    }
    if (proxyUrl && fullUrl.protocol === 'http:') {
        const proxyAuth = getProxyAuthorization(proxyUrl);
        console.log('[QingTian] License request using proxy:', `${proxyUrl.protocol}//${proxyUrl.host}`);
        return {
            client: proxyUrl.protocol === 'https:' ? https : http,
            options: {
                hostname: proxyUrl.hostname,
                port: proxyUrl.port ? parseInt(proxyUrl.port, 10) : (proxyUrl.protocol === 'https:' ? 443 : 80),
                path: fullUrl.toString(),
                method,
                headers: proxyAuth ? { ...headers, 'Proxy-Authorization': proxyAuth } : headers,
                timeout: 10000
            }
        };
    }
    return {
        client: fullUrl.protocol === 'https:' ? https : http,
        options: {
            hostname: fullUrl.hostname,
            port: targetPort,
            path: fullUrl.pathname + fullUrl.search,
            method,
            headers,
            timeout: 10000
        }
    };
}
function getFriendlyNetworkErrorMessage(error) {
    const raw = error instanceof Error ? error.message : String(error || '');
    const lower = raw.toLowerCase();
    if (lower.includes('proxy')) {
        return '网络连接授权服务器失败，请检查代理/VPN 是否可用后重试';
    }
    if (lower.includes('timeout') || lower.includes('timed out') || lower.includes('etimedout')) {
        return '网络连接授权服务器超时，请检查网络、代理/VPN 后重试';
    }
    return '网络连接授权服务器失败，请检查网络、代理/VPN、防火墙后重试';
}
function httpPost(urlPath, body, baseOverride) {
    return new Promise((resolve, reject) => {
        const fullUrl = buildApiUrl(urlPath, baseOverride);
        const postData = JSON.stringify(body);
        const { client, options } = buildRequestOptions(fullUrl, 'POST', {
            'Content-Type': 'application/json',
            'Content-Length': Buffer.byteLength(postData),
            'User-Agent': 'QingTianMcp/' + getVersion()
        });
        const req = client.request(options, (res) => {
            let data = '';
            res.on('data', (chunk) => (data += chunk.toString('utf8')));
            res.on('end', () => {
                const status = res.statusCode ?? 0;
                const raw = data.trim();
                if (!raw) {
                    reject(new Error(`空响应 (HTTP ${status})`));
                    return;
                }
                try {
                    resolve(JSON.parse(raw));
                }
                catch {
                    const snippet = raw.length > 160 ? raw.slice(0, 160) + '…' : raw;
                    reject(new Error(`服务端返回非 JSON (HTTP ${status})，请核对 licenseApiBase 与网关是否指向 RuoYi 接口。片段: ${snippet.replace(/\s+/g, ' ')}`));
                }
            });
        });
        req.on('error', (e) => reject(e));
        req.on('timeout', () => { req.destroy(); reject(new Error('Request timeout')); });
        req.write(postData);
        req.end();
    });
}
function getConfiguredLicenseApiBase() {
    try {
        return (vscode.workspace.getConfiguration('qingtian').get('licenseApiBase') || '').trim().replace(/\/$/, '');
    }
    catch (_) {
        return '';
    }
}
function getExtensionPath() {
    const ext = vscode.extensions.getExtension('QingTian.qingtian-v2') ||
        vscode.extensions.getExtension('qingtian.qingtian-v2') ||
        vscode.extensions.getExtension('local.qingtian-mcp');
    return ext?.extensionPath || null;
}
function hashString(text) {
    return crypto.createHash('sha256').update(text).digest('hex');
}
function hashFile(filePath) {
    try {
        if (!fs.existsSync(filePath)) {
            return undefined;
        }
        return crypto.createHash('sha256').update(fs.readFileSync(filePath)).digest('hex');
    }
    catch (_) {
        return undefined;
    }
}
const PACKAGE_BINDING_MIN_VERSION = '3.6.8';
const PACKAGE_BINDING_MESSAGE = '安装包与激活信息不一致，请重新从官方下载页面下载对应激活码的专属插件包。';
function normalizeWatermarkField(value) {
    const text = typeof value === 'string' ? value.trim() : '';
    return text || undefined;
}
function readPackageWatermark() {
    const extPath = getExtensionPath();
    if (!extPath) {
        return {};
    }
    const jsonPath = path.join(extPath, 'resources', 'qingtian-watermark.json');
    try {
        if (fs.existsSync(jsonPath)) {
            const parsed = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
            return {
                type: normalizeWatermarkField(parsed.type),
                watermarkId: normalizeWatermarkField(parsed.watermarkId),
                codeHash: normalizeWatermarkField(parsed.codeHash),
                version: normalizeWatermarkField(parsed.version),
                generatedAt: normalizeWatermarkField(parsed.generatedAt),
                signature: normalizeWatermarkField(parsed.signature),
                packageSha256: normalizeWatermarkField(parsed.packageSha256)
            };
        }
    }
    catch (_) {
        return { rawInvalid: true };
    }
    const plainPath = path.join(extPath, '.qingtian-watermark');
    try {
        if (fs.existsSync(plainPath)) {
            return { watermarkId: fs.readFileSync(plainPath, 'utf8').trim() };
        }
    }
    catch (_) {
        return { rawInvalid: true };
    }
    return {};
}
function versionCompare(left, right) {
    const a = left.split(/[^\d]+/).filter(Boolean).map((part) => Number(part));
    const b = right.split(/[^\d]+/).filter(Boolean).map((part) => Number(part));
    const max = Math.max(a.length, b.length);
    for (let i = 0; i < max; i++) {
        const av = Number.isFinite(a[i]) ? a[i] : 0;
        const bv = Number.isFinite(b[i]) ? b[i] : 0;
        if (av !== bv) {
            return av > bv ? 1 : -1;
        }
    }
    return 0;
}
function isPackageBindingRequired() {
    return versionCompare(getVersion(), PACKAGE_BINDING_MIN_VERSION) >= 0;
}
function hasPackageWatermark(watermark) {
    return Boolean(watermark.watermarkId ||
        watermark.codeHash ||
        watermark.version ||
        watermark.generatedAt ||
        watermark.signature ||
        watermark.packageSha256);
}
function isStrictPackageWatermark(watermark) {
    return Boolean(watermark.watermarkId &&
        watermark.codeHash &&
        watermark.version &&
        watermark.generatedAt &&
        watermark.signature);
}
function normalizeHash(value) {
    return String(value || '').trim().toLowerCase();
}
function hashActivationCode(code) {
    return hashString(code.trim().toUpperCase());
}
function checkPackageWatermarkState() {
    const watermark = readPackageWatermark();
    const required = isPackageBindingRequired();
    const present = hasPackageWatermark(watermark);
    if (watermark.rawInvalid) {
        return {
            ok: false,
            message: PACKAGE_BINDING_MESSAGE,
            eventType: 'invalid_package_watermark',
            watermark
        };
    }
    if (!present) {
        if (!required) {
            return { ok: true, required, present, watermark };
        }
        return {
            ok: false,
            message: PACKAGE_BINDING_MESSAGE,
            eventType: 'missing_package_watermark',
            watermark
        };
    }
    if (!isStrictPackageWatermark(watermark)) {
        return {
            ok: false,
            message: PACKAGE_BINDING_MESSAGE,
            eventType: 'incomplete_package_watermark',
            watermark
        };
    }
    if (watermark.version !== getVersion()) {
        return {
            ok: false,
            message: PACKAGE_BINDING_MESSAGE,
            eventType: 'package_version_mismatch',
            watermark
        };
    }
    return { ok: true, required, present, watermark };
}
function appendPackageBindingPayload(payload, watermark = readPackageWatermark()) {
    if (watermark.watermarkId) {
        payload.packageWatermarkId = watermark.watermarkId;
        payload.watermarkId = watermark.watermarkId;
    }
    if (watermark.codeHash) {
        payload.packageCodeHash = watermark.codeHash;
    }
    if (watermark.version) {
        payload.packageVersion = watermark.version;
    }
    if (watermark.generatedAt) {
        payload.packageGeneratedAt = watermark.generatedAt;
    }
    if (watermark.signature) {
        payload.packageSignature = watermark.signature;
    }
}
function applyPackageBindingToLicense(license, watermark = readPackageWatermark()) {
    if (!isStrictPackageWatermark(watermark)) {
        return license;
    }
    return {
        ...license,
        packageWatermarkId: watermark.watermarkId,
        packageCodeHash: normalizeHash(watermark.codeHash),
        packageVersion: watermark.version,
        packageGeneratedAt: watermark.generatedAt,
        packageSignature: watermark.signature,
        packageBoundAt: Date.now()
    };
}
function storedPackageBindingMatches(license, watermark) {
    if (!isStrictPackageWatermark(watermark)) {
        return true;
    }
    return (license.packageWatermarkId === watermark.watermarkId &&
        normalizeHash(license.packageCodeHash) === normalizeHash(watermark.codeHash) &&
        license.packageVersion === watermark.version &&
        license.packageGeneratedAt === watermark.generatedAt &&
        license.packageSignature === watermark.signature);
}
function checkPackageBindingForCode(code, state) {
    if (!state.ok) {
        return { ok: false, message: state.message, eventType: state.eventType, watermark: state.watermark };
    }
    if (!state.present) {
        return { ok: true, requiresOnlineVerify: false, watermark: state.watermark };
    }
    if (normalizeHash(state.watermark.codeHash) !== hashActivationCode(code)) {
        return {
            ok: false,
            message: PACKAGE_BINDING_MESSAGE,
            eventType: 'activation_code_package_mismatch',
            watermark: state.watermark
        };
    }
    return { ok: true, requiresOnlineVerify: false, watermark: state.watermark };
}
function checkPackageBindingForLicense(license, state) {
    if (!state.ok) {
        return { ok: false, message: state.message, eventType: state.eventType, watermark: state.watermark };
    }
    if (!state.present) {
        return { ok: true, requiresOnlineVerify: false, watermark: state.watermark };
    }
    if (license.code) {
        return checkPackageBindingForCode(license.code, state);
    }
    if (!storedPackageBindingMatches(license, state.watermark)) {
        return { ok: true, requiresOnlineVerify: true, watermark: state.watermark };
    }
    return { ok: true, requiresOnlineVerify: false, watermark: state.watermark };
}
function reportPackageBindingFailure(eventType, watermark) {
    void reportSecurityEvent(eventType, {
        watermarkId: watermark.watermarkId,
        packageVersion: watermark.version,
        packageGeneratedAt: watermark.generatedAt
    });
}
function shouldReportSecurityEvent(eventKey) {
    const cacheFile = path.join(LICENSE_DIR, 'security-report-cache.json');
    const today = new Date().toISOString().slice(0, 10);
    let cache = {};
    try {
        if (fs.existsSync(cacheFile)) {
            cache = JSON.parse(fs.readFileSync(cacheFile, 'utf8'));
        }
    }
    catch (_) {
        cache = {};
    }
    if (cache[eventKey] === today) {
        return false;
    }
    cache[eventKey] = today;
    try {
        if (!fs.existsSync(LICENSE_DIR)) {
            fs.mkdirSync(LICENSE_DIR, { recursive: true });
        }
        fs.writeFileSync(cacheFile, JSON.stringify(cache), 'utf8');
    }
    catch (_) { }
    return true;
}
async function reportSecurityEvent(eventType, extra = {}) {
    try {
        const configuredApiBase = String(extra.configuredApiBase || getConfiguredLicenseApiBase() || '');
        const watermark = readPackageWatermark();
        const extPath = getExtensionPath();
        const manifestHash = extPath ? hashFile(path.join(extPath, 'package.json')) : undefined;
        const watermarkId = String(extra.watermarkId || watermark.watermarkId || '');
        const eventKey = [
            eventType,
            watermarkId,
            hashString(configuredApiBase || '-'),
            getVersion()
        ].join(':');
        if (!shouldReportSecurityEvent(eventKey)) {
            return;
        }
        await httpPost('/api/qt/security/report', {
            eventType,
            watermarkId: watermarkId || undefined,
            machineIdHash: hashString(getMachineId()),
            configuredApiBase,
            version: getVersion(),
            buildId: watermarkId || undefined,
            manifestHash,
            packageSha256: extra.packageSha256 || watermark.packageSha256,
            flags: {
                ...extra,
                watermarkInvalid: watermark.rawInvalid || undefined
            }
        }, OFFICIAL_LICENSE_API_BASE);
    }
    catch (e) {
        console.warn('[QingTian] security report skipped:', e.message);
    }
}
function reportStartupSecurityState() {
    const configuredApiBase = getConfiguredLicenseApiBase();
    if (configuredApiBase && !validateEndpoint(configuredApiBase)) {
        void reportSecurityEvent('untrusted_api_base', { configuredApiBase });
    }
    const packageState = checkPackageWatermarkState();
    if (!packageState.ok) {
        void reportSecurityEvent(packageState.eventType, {
            configuredApiBase,
            watermarkId: packageState.watermark.watermarkId,
            packageVersion: packageState.watermark.version,
            packageGeneratedAt: packageState.watermark.generatedAt
        });
    }
}
// ===== 公开API =====
let _activated = false;
let _verifyTimer = null;
let _activationInvalidHandler = null;
let _serverConnectionHandler = null;
let _serverDisconnected = false;
let _firstDisconnectTime = null;
const VERIFY_INTERVAL_MS = 6 * 60 * 60 * 1000;
const RECONNECT_INTERVAL_MS = 15 * 60 * 1000;
const MAX_OFFLINE_DURATION_MS = 7 * 24 * 60 * 60 * 1000; // 最大离线时长 7 天
const NETWORK_BLOCK_MESSAGE = '授权服务器不可用，请检查网络连接后重试';
let _updatePushHandler = null;
let _updatePushStopped = true;
let _updatePushReq = null;
let _updatePushReconnectTimer = null;
function invalidateActivation(message, clearLocalLicense = true) {
    _activated = false;
    stopPeriodicVerify();
    if (clearLocalLicense) {
        clearLicense();
    }
    _activationInvalidHandler?.(message);
}
function getVersion() {
    const ext = vscode.extensions.getExtension('QingTian.qingtian-v2') ||
        vscode.extensions.getExtension('qingtian.qingtian-v2') ||
        vscode.extensions.getExtension('local.qingtian-mcp');
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const fallback = require('../package.json').version;
    return ext?.packageJSON?.version ?? fallback;
}
function setActivationInvalidHandler(handler) {
    _activationInvalidHandler = handler;
}
function setServerConnectionHandler(handler) {
    _serverConnectionHandler = handler;
}
function setUpdatePushHandler(handler) {
    _updatePushHandler = handler;
}
function isServerDisconnected() {
    return _serverDisconnected;
}
function clearUpdatePushReconnectTimer() {
    if (_updatePushReconnectTimer) {
        clearTimeout(_updatePushReconnectTimer);
        _updatePushReconnectTimer = null;
    }
}
function scheduleUpdatePushReconnect(reason) {
    if (_updatePushStopped || _updatePushReconnectTimer) {
        return;
    }
    console.log('[QingTian] 更新推送连接中断，将在 15 秒后重连:', reason);
    _updatePushReconnectTimer = setTimeout(() => {
        _updatePushReconnectTimer = null;
        connectUpdatePushStream();
    }, RECONNECT_INTERVAL_MS);
}
function pickNoticeField(payload, names) {
    if (!payload || typeof payload !== 'object') {
        return undefined;
    }
    for (const name of names) {
        const value = payload[name];
        if (value !== undefined && value !== null && String(value).trim() !== '') {
            return value;
        }
    }
    return undefined;
}
function normalizeUpdateNoticePayload(payload) {
    const notice = {
        ...(payload || {}),
        id: pickNoticeField(payload, ['id', 'noticeId', 'notice_id']),
        title: String(pickNoticeField(payload, ['title', 'noticeTitle', 'notice_title']) ?? ''),
        content: String(pickNoticeField(payload, ['content', 'noticeContent', 'notice_content', 'body']) ?? ''),
        actionText: String(pickNoticeField(payload, ['actionText', 'action_text', 'buttonText', 'button_text', 'downloadText', 'download_text']) ?? ''),
        actionUrl: String(pickNoticeField(payload, ['actionUrl', 'action_url', 'downloadUrl', 'download_url', 'buttonUrl', 'button_url', 'link', 'url', 'href']) ?? ''),
        publishTime: pickNoticeField(payload, ['publishTime', 'publish_time', 'publishedAt', 'published_at', 'createTime', 'create_time'])
    };
    return notice;
}
function parseUpdatePushEvent(rawEvent) {
    const lines = rawEvent.split(/\r?\n/);
    let eventName = 'message';
    const dataLines = [];
    for (const line of lines) {
        if (!line) {
            continue;
        }
        if (line.startsWith(':')) {
            continue;
        }
        if (line.startsWith('event:')) {
            eventName = line.slice(6).trim();
            continue;
        }
        if (line.startsWith('data:')) {
            dataLines.push(line.slice(5).trim());
        }
    }
    if (dataLines.length === 0) {
        return null;
    }
    try {
        const payload = JSON.parse(dataLines.join('\n'));
        if (eventName === 'snapshot' || eventName === 'published') {
            return {
                type: eventName,
                notice: normalizeUpdateNoticePayload(payload)
            };
        }
        if (eventName === 'recalled') {
            return {
                type: 'recalled',
                noticeId: payload?.id ?? null
            };
        }
        return null;
    }
    catch (e) {
        console.warn('[QingTian] 解析更新推送事件失败', e.message);
        return null;
    }
}
function connectUpdatePushStream() {
    if (_updatePushStopped || _updatePushReq) {
        return;
    }
    const fullUrl = buildApiUrl('/api/qt/updates/stream');
    const { client, options } = buildRequestOptions(fullUrl, 'GET', {
        Accept: 'text/event-stream',
        'Cache-Control': 'no-cache',
        'User-Agent': 'QingTianMcp/' + getVersion()
    });
    const req = client.request(options);
    _updatePushReq = req;
    req.on('response', (res) => {
        if ((res.statusCode ?? 0) !== 200) {
            const status = res.statusCode ?? 0;
            console.warn('[QingTian] 更新推送连接失败 HTTP', status);
            res.resume();
            if (_updatePushReq === req) {
                _updatePushReq = null;
            }
            scheduleUpdatePushReconnect('HTTP ' + status);
            return;
        }
        console.log('[QingTian] 更新推送已连接');
        let buffer = '';
        let finalized = false;
        const finalize = (reason) => {
            if (finalized) {
                return;
            }
            finalized = true;
            if (_updatePushReq === req) {
                _updatePushReq = null;
            }
            scheduleUpdatePushReconnect(reason);
        };
        res.setEncoding('utf8');
        res.on('data', (chunk) => {
            buffer += chunk;
            while (true) {
                const match = buffer.match(/([\s\S]*?)(?:\r?\n\r?\n)/);
                if (!match) {
                    break;
                }
                buffer = buffer.slice(match[0].length);
                const event = parseUpdatePushEvent(match[1]);
                if (event) {
                    _updatePushHandler?.(event);
                }
            }
        });
        res.on('end', () => finalize('stream ended'));
        res.on('close', () => finalize('stream closed'));
        res.on('error', (e) => finalize(e.message));
    });
    req.on('socket', (socket) => {
        socket.setKeepAlive(true, 30 * 1000);
    });
    req.on('error', (e) => {
        if (_updatePushReq === req) {
            _updatePushReq = null;
        }
        if (_updatePushStopped) {
            return;
        }
        scheduleUpdatePushReconnect(e.message);
    });
    req.end();
}
function startUpdatePush() {
    _updatePushStopped = false;
    clearUpdatePushReconnectTimer();
    connectUpdatePushStream();
}
function stopUpdatePush() {
    _updatePushStopped = true;
    clearUpdatePushReconnectTimer();
    if (_updatePushReq) {
        try {
            _updatePushReq.destroy();
        }
        catch { }
        _updatePushReq = null;
    }
}
function buildVerifyPayload(license, options) {
    const includeTicket = options?.includeTicket !== false;
    const payload = {
        machineId: license.machineId,
        version: getVersion()
    };
    if (license.refreshToken) {
        payload.refreshToken = license.refreshToken;
    }
    else if (license.code) {
        payload.code = license.code;
    }
    const ticketUsable = Boolean(license.ticket &&
        (!license.ticketExpiresAt || license.ticketExpiresAt > Date.now() + 5000));
    if (includeTicket && ticketUsable && license.ticket) {
        payload.ticket = license.ticket;
    }
    appendPackageBindingPayload(payload);
    return payload;
}
function applyServerLicenseData(license, result) {
    const next = {
        ...license,
        lastVerified: Date.now()
    };
    if (result.refreshToken) {
        next.refreshToken = String(result.refreshToken);
        delete next.code;
    }
    if (result.ticket) {
        next.ticket = String(result.ticket);
    }
    if (result.ticketExpiresAt) {
        next.ticketExpiresAt = Number(result.ticketExpiresAt);
    }
    if (result.offlineLicense) {
        next.offlineLicense = String(result.offlineLicense);
    }
    if (Object.prototype.hasOwnProperty.call(result, 'expiresAt')) {
        next.expiresAt = result.expiresAt ? new Date(result.expiresAt).getTime() : null;
    }
    if (Object.prototype.hasOwnProperty.call(result, 'durationType')) {
        next.durationType = Number(result.durationType || 0);
    }
    if (result.policy) {
        next.policy = normalizePolicy(result.policy);
    }
    if (result.serverTime) {
        next.serverTime = Number(result.serverTime);
        next.lastSyncTime = Date.now();
    }
    return next;
}
function shouldTreatVerifyFailureAsTransient(message, license) {
    const text = (message || '').toLowerCase();
    const hardInvalidKeywords = [
        'device mismatch',
        'machine mismatch',
        'disabled',
        'forbidden',
        'expired',
        'invalid code',
        'package',
        'watermark',
        'binding',
        '安装包',
        '包体',
        '水印',
        '专属包',
        '设备不匹配',
        '状态异常',
        '禁用',
        '过期',
        '激活码无效',
        '其他设备'
    ];
    if (hardInvalidKeywords.some((k) => text.includes(k))) {
        return false;
    }
    const transientKeywords = [
        'ticket',
        'token',
        'gateway',
        'timeout',
        'timed out',
        'temporary',
        'temporarily',
        'unavailable',
        'network',
        'proxy',
        'cloudflare',
        '429',
        'rate limit',
        '票据',
        '令牌',
        '网关',
        '超时',
        '网络',
        '不可用',
        '连接',
        '频繁'
    ];
    if (transientKeywords.some((k) => text.includes(k))) {
        return true;
    }
    // Migration path: users not yet holding offline license should not be kicked
    // for ambiguous verify failures (especially in cross-region networks).
    return Boolean(license.refreshToken && !license.offlineLicense);
}
async function verifyLicenseOnline(license) {
    try {
        const firstPayload = buildVerifyPayload(license);
        let result = await httpPost('/api/qt/verify', firstPayload);
        if (result.encryptionKey) {
            _serverEncryptionKey = result.encryptionKey;
        }
        // Some deployed backends still hard-fail when a short-lived ticket expires.
        // Retry once without ticket to avoid false "activation invalid" kicks.
        if (result.valid !== true && firstPayload.ticket) {
            try {
                result = await httpPost('/api/qt/verify', buildVerifyPayload(license, { includeTicket: false }));
                if (result.encryptionKey) {
                    _serverEncryptionKey = result.encryptionKey;
                }
            }
            catch {
                return {
                    ok: false,
                    message: NETWORK_BLOCK_MESSAGE,
                    clearLocalLicense: false
                };
            }
        }
        if (result.valid === true) {
            return { ok: true, license: applyPackageBindingToLicense(applyServerLicenseData(license, result)) };
        }
        const transientFailure = shouldTreatVerifyFailureAsTransient(String(result.message || ''), license);
        return {
            ok: false,
            message: result.message || '激活码已失效',
            clearLocalLicense: !transientFailure
        };
    }
    catch {
        return {
            ok: false,
            message: NETWORK_BLOCK_MESSAGE,
            clearLocalLicense: false
        };
    }
}
function isActivated() {
    return _activated;
}
/**
 * 启动时检查激活状态
 */
async function checkActivation() {
    reportStartupSecurityState();
    const license = loadLicense();
    if (!license) {
        _activated = false;
        stopPeriodicVerify();
        return false;
    }
    const packageState = checkPackageWatermarkState();
    if (!packageState.ok) {
        reportPackageBindingFailure(packageState.eventType, packageState.watermark);
        invalidateActivation(packageState.message);
        return false;
    }
    const machineId = getMachineId();
    const offlineValidation = license.offlineLicense ? validateOfflineLicense(license, machineId) : null;
    if (offlineValidation?.ok && offlineValidation.payload) {
        const trustedLicense = applyOfflineLicensePayload(license, offlineValidation.payload);
        const packageBinding = checkPackageBindingForLicense(trustedLicense, packageState);
        if (!packageBinding.ok) {
            reportPackageBindingFailure(packageBinding.eventType, packageBinding.watermark);
            invalidateActivation(packageBinding.message);
            return false;
        }
        const boundTrustedLicense = applyPackageBindingToLicense(trustedLicense, packageBinding.watermark);
        if (boundTrustedLicense.code !== license.code ||
            boundTrustedLicense.machineId !== license.machineId ||
            boundTrustedLicense.activatedAt !== license.activatedAt ||
            boundTrustedLicense.expiresAt !== license.expiresAt ||
            boundTrustedLicense.durationType !== license.durationType ||
            boundTrustedLicense.packageWatermarkId !== license.packageWatermarkId ||
            boundTrustedLicense.packageCodeHash !== license.packageCodeHash ||
            boundTrustedLicense.packageVersion !== license.packageVersion ||
            boundTrustedLicense.packageGeneratedAt !== license.packageGeneratedAt ||
            boundTrustedLicense.packageSignature !== license.packageSignature) {
            saveLicense(boundTrustedLicense);
        }
        _activated = true;
        _serverDisconnected = false;
        stopPeriodicVerify();
        return true;
    }
    if (license.offlineLicense && offlineValidation && !offlineValidation.ok) {
        invalidateActivation(offlineValidation.message || '离线许可证无效');
        return false;
    }
    const correctedNow = calculateServerTime(license);
    if (license.expiresAt && license.expiresAt < correctedNow) {
        console.log('[QingTian] 本地检测激活码已过期');
        invalidateActivation('激活码已过期');
        return false;
    }
    if (!license.refreshToken && !license.code) {
        invalidateActivation('本地授权数据不完整，请重新激活');
        return false;
    }
    const packageBinding = checkPackageBindingForLicense(license, packageState);
    if (!packageBinding.ok) {
        reportPackageBindingFailure(packageBinding.eventType, packageBinding.watermark);
        invalidateActivation(packageBinding.message);
        return false;
    }
    const verifyResult = await verifyLicenseOnline(license);
    if (!verifyResult.ok) {
        console.log('[QingTian] 联网验证失败:', verifyResult.message);
        // 网络错误且本地 license 未过期 → 离线模式启动
        if (packageBinding.requiresOnlineVerify) {
            invalidateActivation(PACKAGE_BINDING_MESSAGE, false);
            return false;
        }
        if (!verifyResult.clearLocalLicense) {
            const correctedNow2 = calculateServerTime(license);
            const localValid = !license.expiresAt || license.expiresAt > correctedNow2;
            if (localValid) {
                console.log('[QingTian] 服务端暂不可达，继续使用本地授权并等待后续迁移离线许可证');
                _activated = true;
                _serverDisconnected = true;
                _serverConnectionHandler?.(false, '授权服务暂不可用，继续使用本地授权');
                stopPeriodicVerify();
                return true;
            }
        }
        invalidateActivation(verifyResult.message, verifyResult.clearLocalLicense);
        return false;
    }
    const verifiedLicense = verifyResult.license;
    const migratedOffline = verifiedLicense.offlineLicense ? validateOfflineLicense(verifiedLicense, machineId) : null;
    if (verifiedLicense.offlineLicense && migratedOffline && !migratedOffline.ok) {
        invalidateActivation(migratedOffline.message || '离线许可证校验失败');
        return false;
    }
    const finalLicense = migratedOffline?.ok && migratedOffline.payload
        ? applyOfflineLicensePayload(verifiedLicense, migratedOffline.payload)
        : verifiedLicense;
    saveLicense(applyPackageBindingToLicense(finalLicense, packageBinding.watermark));
    _activated = true;
    _serverDisconnected = false;
    stopPeriodicVerify();
    return true;
}
/**
 * 激活
 */
async function activate(code) {
    reportStartupSecurityState();
    const normalizedCode = code.trim().toUpperCase();
    const packageState = checkPackageWatermarkState();
    if (!packageState.ok) {
        reportPackageBindingFailure(packageState.eventType, packageState.watermark);
        return { success: false, message: packageState.message };
    }
    const packageBinding = checkPackageBindingForCode(normalizedCode, packageState);
    if (!packageBinding.ok) {
        reportPackageBindingFailure(packageBinding.eventType, packageBinding.watermark);
        return { success: false, message: packageBinding.message };
    }
    const machineId = getMachineId();
    const osInfo = `${os.platform()} ${os.release()} ${os.arch()}`;
    const hostname = os.hostname();
    try {
        const payload = {
            code: normalizedCode,
            machineId,
            osInfo,
            hostname,
            version: getVersion()
        };
        appendPackageBindingPayload(payload, packageBinding.watermark);
        const result = await httpPost('/api/qt/activate', payload);
        if (result.success === true) {
            const license = {
                refreshToken: result.refreshToken ? String(result.refreshToken) : undefined,
                ticket: result.ticket ? String(result.ticket) : undefined,
                ticketExpiresAt: result.ticketExpiresAt ? Number(result.ticketExpiresAt) : undefined,
                offlineLicense: result.offlineLicense ? String(result.offlineLicense) : undefined,
                machineId,
                activatedAt: Date.now(),
                expiresAt: result.expiresAt ? new Date(result.expiresAt).getTime() : null,
                durationType: result.durationType || 0,
                lastVerified: Date.now(),
                serverTime: result.serverTime || Date.now(),
                lastSyncTime: Date.now(),
                policy: normalizePolicy(result.policy)
            };
            if (!license.refreshToken) {
                license.code = normalizedCode;
            }
            const offlineValidation = license.offlineLicense ? validateOfflineLicense(license, machineId) : null;
            if (offlineValidation && !offlineValidation.ok) {
                return { success: false, message: offlineValidation.message || '离线许可证校验失败' };
            }
            const finalLicense = offlineValidation?.payload
                ? applyOfflineLicensePayload(license, offlineValidation.payload)
                : license;
            saveLicense(applyPackageBindingToLicense(finalLicense, packageBinding.watermark));
            _activated = true;
            _serverDisconnected = false;
            stopPeriodicVerify();
            return { success: true, message: result.message || '激活成功' };
        }
        else {
            return { success: false, message: result.message || '激活失败' };
        }
    }
    catch (e) {
        console.warn('[QingTian] Activation network error:', e);
        return { success: false, message: getFriendlyNetworkErrorMessage(e) };
    }
}
function getLicenseInfo() {
    return loadLicense();
}
function getExpiresText() {
    const license = loadLicense();
    if (!license)
        return '未激活';
    if (!license.expiresAt)
        return '永不过期';
    const d = new Date(license.expiresAt);
    return d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0') + ' ' +
        String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
}
function getLicenseCountdownStatus() {
    const license = loadLicense();
    if (!license || !_activated) {
        return {
            activated: false,
            permanent: false,
            expiresAt: null,
            remainingMs: null
        };
    }
    if (!license.expiresAt) {
        return {
            activated: true,
            permanent: true,
            expiresAt: null,
            remainingMs: null
        };
    }
    const remainingMs = Math.max(0, license.expiresAt - calculateServerTime(license));
    return {
        activated: true,
        permanent: false,
        expiresAt: license.expiresAt,
        remainingMs
    };
}
/**
 * 定期验证（低频兜底）
 */
function startPeriodicVerify(license) {
    if (_verifyTimer) {
        clearInterval(_verifyTimer);
    }
    const interval = _serverDisconnected ? RECONNECT_INTERVAL_MS : VERIFY_INTERVAL_MS;
    _verifyTimer = setInterval(async () => {
        const packageState = checkPackageWatermarkState();
        if (!packageState.ok) {
            reportPackageBindingFailure(packageState.eventType, packageState.watermark);
            invalidateActivation(packageState.message);
            return;
        }
        const packageBinding = checkPackageBindingForLicense(license, packageState);
        if (!packageBinding.ok) {
            reportPackageBindingFailure(packageBinding.eventType, packageBinding.watermark);
            invalidateActivation(packageBinding.message);
            return;
        }
        const verifyResult = await verifyLicenseOnline(license);
        if (verifyResult.ok) {
            Object.assign(license, verifyResult.license);
            _activated = true;
            saveLicense(license);
            // 从断线恢复
            if (_serverDisconnected) {
                console.log('[QingTian] 服务器已恢复连接');
                _serverDisconnected = false;
                _firstDisconnectTime = null;
                _serverConnectionHandler?.(true, '服务器已恢复连接');
                // 切换回正常验证间隔
                startPeriodicVerify(license);
            }
            return;
        }
        console.log('[QingTian] 定期验证失败:', verifyResult.message);
        // 网络错误 → 不 invalidate，标记断线并继续重试
        if (packageBinding.requiresOnlineVerify) {
            invalidateActivation(PACKAGE_BINDING_MESSAGE, false);
            return;
        }
        if (!verifyResult.clearLocalLicense) {
            const correctedNow = calculateServerTime(license);
            const localValid = !license.expiresAt || license.expiresAt > correctedNow;
            if (localValid) {
                if (!_serverDisconnected) {
                    console.log('[QingTian] 服务器不可达，进入断线重连模式');
                    _serverDisconnected = true;
                    _firstDisconnectTime = Date.now();
                    _serverConnectionHandler?.(false, '服务器不可达，自动重连中...');
                    // 切换到更长的重连间隔
                    startPeriodicVerify(license);
                }
                else if (_firstDisconnectTime && (Date.now() - _firstDisconnectTime > MAX_OFFLINE_DURATION_MS)) {
                    // 超过最大离线时长，强制失活
                    console.log('[QingTian] 离线超过24小时，强制失活');
                    _firstDisconnectTime = null;
                    invalidateActivation('离线时间过长，请连接网络后重新验证', false);
                    return;
                }
                return;
            }
        }
        // 真正的激活失效（服务器返回 invalid 或本地过期）
        invalidateActivation(verifyResult.message, verifyResult.clearLocalLicense);
    }, interval);
}
function stopPeriodicVerify() {
    if (_verifyTimer) {
        clearInterval(_verifyTimer);
        _verifyTimer = null;
    }
}
//# sourceMappingURL=activation.js.map