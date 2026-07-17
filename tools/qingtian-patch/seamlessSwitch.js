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
exports.registerBatchEngine = registerBatchEngine;
exports.adoptFocusedWindowForBatch = adoptFocusedWindowForBatch;
exports.getSeamlessInjectionStatus = getSeamlessInjectionStatus;
exports.isInjected = isInjected;
exports.isStartPromptAutomationInjected = isStartPromptAutomationInjected;
exports.isComposerBridgeInjected = isComposerBridgeInjected;
exports.isComposerServiceHooked = isComposerServiceHooked;
exports.refreshPrimaryRuntimeProbe = refreshPrimaryRuntimeProbe;
exports.getSeamlessRuntimeStatus = getSeamlessRuntimeStatus;
exports.injectWorkbench = injectWorkbench;
exports.restoreWorkbench = restoreWorkbench;
exports.startServer = startServer;
exports.stopServer = stopServer;
exports.setPendingSwitch = setPendingSwitch;
exports.sendStartPromptToCursor = sendStartPromptToCursor;
exports.hasPendingSwitch = hasPendingSwitch;
exports.getSeamlessPort = getSeamlessPort;
/**
 * 无感切号模块
 *
 * 原理：
 * 1. 注入 Cursor 的 workbench.desktop.main.js，hook AuthService 暴露到 window.__qtAuthService
 * 2. 注入的脚本轮询本地 HTTP 服务器获取待切换 token
 * 3. 拿到 token 后直接修改内存中的 localOverrideAccessToken，无需重启
 */
const vscode = __importStar(require("vscode"));
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
const http = __importStar(require("http"));
const crypto = __importStar(require("crypto"));
const child_process_1 = require("child_process");
// ─── 常量 ─────────────────────────────────────────
const SEAMLESS_MARKER = '/* __QINGTIAN_SEAMLESS__ */';
const SEAMLESS_START_PROMPT_MARKER = '/* __QINGTIAN_START_PROMPT_AUTOMATION_V7__ */';
const SEAMLESS_COMPOSER_BRIDGE_MARKER = '/* __QINGTIAN_COMPOSER_BRIDGE_V2__ */';
const COMPOSER_SERVICE_HOOK_MARKER = '/* __QINGTIAN_COMPOSER_SERVICE_HOOK_V2__ */';
const SEAMLESS_RUNTIME_VERSION = '20260612-busy-wait-buildsync';
const SEAMLESS_PORT = 36530;
const POLL_INTERVAL = 500; // 注入脚本轮询间隔 ms
// ─── 状态 ─────────────────────────────────────────
let httpServer = null;
let pendingSwitch = null;
let pendingSwitchWaiter = null;
let pendingStartPrompt = null;
const channelComposerBindings = {};
let serverPort = SEAMLESS_PORT;
const seamlessRuntimeClients = {};
const SEAMLESS_RUNTIME_CLIENT_TTL_MS = 5000;
// 多工作区/多窗口场景：注入是全局的（写死注入脚本里的心跳端口 = primary 36530），
// 所有窗口的 runtime 心跳都发往 primary 实例。本实例若是 fallback（serverPort !== SEAMLESS_PORT），
// 自身 seamlessRuntimeClients 恒为空，会把“注入已生效”误判为“待重启”。
// 这里缓存一次向 primary 探活的结果，供 getSeamlessRuntimeStatus 合并使用。
let primaryRuntimeProbe = { loaded: false, anyLoaded: false, count: 0, at: 0 };
const PRIMARY_RUNTIME_PROBE_TTL_MS = 15000;
// ── 多工作区批量重试：窗口↔工作区引擎亲和 ──
// 由 primary(36530) 充当协调中心：各工作区引擎注册自己的桥端口；
// 触发批量重试时把“当前焦点窗口”认领给该引擎；注入脚本据此连到自己工作区的桥。
const batchEngineRegistry = {};
const batchBridgeAdoptions = {};
const BATCH_ADOPTION_TTL_MS = 10 * 60 * 1000;
function workspaceScopeId(workspacePath) {
    const raw = String(workspacePath || '').trim();
    if (!raw)
        return '';
    const normalized = process.platform === 'win32' ? raw.toLowerCase() : raw;
    return crypto.createHash('sha256').update(normalized).digest('hex').slice(0, 16);
}
function registerBatchEngineLocal(port, workspacePath) {
    const p = Number(port) || 0;
    if (!p)
        return;
    batchEngineRegistry[p] = { port: p, workspacePath: String(workspacePath || ''), at: Date.now() };
}
function adoptFocusedWindowLocal(port, workspacePath) {
    const p = Number(port) || 0;
    registerBatchEngineLocal(p, workspacePath);
    const now = Date.now();
    pruneSeamlessRuntimeClients(now);
    const focused = Object.values(seamlessRuntimeClients)
        .filter(c => c.focused)
        .sort((a, b) => b.lastSeen - a.lastSeen)[0];
    if (!focused)
        return { ok: false };
    batchBridgeAdoptions[focused.clientId] = { port: p, workspacePath: String(workspacePath || ''), at: now };
    return { ok: true, clientId: focused.clientId };
}
function getBatchBridgeForClient(clientId) {
    const key = String(clientId || '');
    const rec = batchBridgeAdoptions[key];
    if (!rec)
        return 0;
    if (Date.now() - rec.at > BATCH_ADOPTION_TTL_MS) {
        delete batchBridgeAdoptions[key];
        return 0;
    }
    return Number(rec.port) || 0;
}
function getBatchBridgeMetaForClient(clientId) {
    const key = String(clientId || '');
    const rec = batchBridgeAdoptions[key];
    if (!rec || Date.now() - rec.at > BATCH_ADOPTION_TTL_MS) {
        if (rec)
            delete batchBridgeAdoptions[key];
        return { port: 0, workspaceScopeId: '', workspacePath: '' };
    }
    return {
        port: Number(rec.port) || 0,
        workspaceScopeId: workspaceScopeId(rec.workspacePath),
        workspacePath: String(rec.workspacePath || '')
    };
}
// 引擎/扩展侧调用：若本实例是 primary 直接本地处理，否则转发给 primary(36530)。
async function registerBatchEngine(port, workspacePath) {
    if (serverPort === SEAMLESS_PORT) {
        registerBatchEngineLocal(port, workspacePath);
        return;
    }
    await postJsonToLocalPort(SEAMLESS_PORT, '/api/register-batch-engine', { port, workspacePath });
}
async function adoptFocusedWindowForBatch(port, workspacePath) {
    if (serverPort === SEAMLESS_PORT) {
        return adoptFocusedWindowLocal(port, workspacePath);
    }
    const r = await postJsonToLocalPort(SEAMLESS_PORT, '/api/adopt-focused-window', { port, workspacePath });
    return { ok: !!(r.ok && r.data?.ok), clientId: r.data?.clientId };
}
// ─── workbench 路径 ────────────────────────────────
function findWorkbenchJsPath() {
    const candidates = [];
    if (process.platform === 'win32') {
        const local = process.env.LOCALAPPDATA || '';
        if (local) {
            candidates.push(path.join(local, 'Programs', 'Cursor', 'resources', 'app'));
            candidates.push(path.join(local, 'Programs', 'cursor', 'resources', 'app'));
            candidates.push(path.join(local, 'Cursor', 'resources', 'app'));
        }
        candidates.push('C:\\Program Files\\Cursor\\resources\\app');
    }
    else if (process.platform === 'darwin') {
        candidates.push('/Applications/Cursor.app/Contents/Resources/app');
    }
    else {
        candidates.push('/opt/Cursor/resources/app');
        candidates.push('/usr/share/cursor/resources/app');
    }
    // 也可以从当前进程路径推断
    const execDir = path.dirname(process.execPath);
    if (process.platform === 'darwin') {
        candidates.push(path.resolve(execDir, '..', 'Resources', 'app'));
        candidates.push(path.resolve(execDir, '..', '..', 'Resources', 'app'));
    }
    candidates.push(path.join(execDir, 'resources', 'app'));
    // 往上找
    let cur = execDir;
    for (let i = 0; i < 5; i++) {
        const wb = path.join(cur, 'resources', 'app', 'out', 'vs', 'workbench', 'workbench.desktop.main.js');
        if (fs.existsSync(wb)) {
            return wb;
        }
        cur = path.dirname(cur);
    }
    for (const dir of candidates) {
        const wb = path.join(dir, 'out', 'vs', 'workbench', 'workbench.desktop.main.js');
        if (fs.existsSync(wb)) {
            return wb;
        }
    }
    return null;
}
function extractInjectedRuntimeVersion(content) {
    const markerMatch = content.match(/\/\*\s*__QINGTIAN_RUNTIME_VERSION:([^*]+?)__\s*\*\//);
    if (markerMatch?.[1]) {
        return markerMatch[1].trim();
    }
    const markerIdx = content.indexOf(SEAMLESS_MARKER);
    if (markerIdx < 0) {
        return null;
    }
    const runtimeIdx = content.indexOf('window[_QT_RUNTIME_KEY]={version:', markerIdx);
    const scriptSlice = content.slice(runtimeIdx >= 0 ? runtimeIdx : markerIdx, markerIdx + 12000);
    const legacyMatch = scriptSlice.match(/\{version:"([^"]+)"/);
    return legacyMatch?.[1]?.trim() || null;
}
function getSeamlessInjectionStatus() {
    const wp = findWorkbenchJsPath();
    if (!wp || !fs.existsSync(wp)) {
        return { injected: false, current: false, version: null, expectedVersion: SEAMLESS_RUNTIME_VERSION };
    }
    const content = fs.readFileSync(wp, 'utf-8');
    const injected = content.includes(SEAMLESS_MARKER);
    const version = injected ? extractInjectedRuntimeVersion(content) : null;
    return {
        injected,
        current: injected && version === SEAMLESS_RUNTIME_VERSION,
        version,
        expectedVersion: SEAMLESS_RUNTIME_VERSION
    };
}
const DEFAULT_BATCH_RETRY_PORT = 26399;
// ─── 注入脚本生成 ──────────────────────────────────
function buildInjectionScript(port) {
    return `
${SEAMLESS_MARKER}
${SEAMLESS_START_PROMPT_MARKER}
/* __QINGTIAN_RUNTIME_VERSION:${SEAMLESS_RUNTIME_VERSION}__ */
;(function(){
"use strict";
var PORT=${port};
var POLL=${POLL_INTERVAL};
var _qtLog=function(m){try{console.log("[QT-Seamless] "+m)}catch(e){}};
var _QT_RUNTIME_KEY="__qingtianSeamlessRuntime";
try{
    var _oldRuntime=window[_QT_RUNTIME_KEY];
    if(_oldRuntime&&typeof _oldRuntime.stop==="function")_oldRuntime.stop("reinject");
}catch(e){}
var _qtStopped=false;
var _qtIntervals=[];
var _qtTimeouts=[];
function _qtSetInterval(fn,ms){
    var id=setInterval(function(){if(!_qtStopped)fn()},ms);
    _qtIntervals.push(id);
    return id;
}
function _qtSetTimeout(fn,ms){
    var id=setTimeout(function(){
        for(var i=_qtTimeouts.length-1;i>=0;i--){if(_qtTimeouts[i]===id){_qtTimeouts.splice(i,1);break}}
        if(!_qtStopped)fn();
    },ms);
    _qtTimeouts.push(id);
    return id;
}
function _qtStop(reason){
    _qtStopped=true;
    for(var i=0;i<_qtIntervals.length;i++){try{clearInterval(_qtIntervals[i])}catch(e){}}
    for(var j=0;j<_qtTimeouts.length;j++){try{clearTimeout(_qtTimeouts[j])}catch(e){}}
    _qtIntervals=[];
    _qtTimeouts=[];
    try{console.log("[QT-Seamless] stopped "+(reason||""))}catch(e){}
}
try{window[_QT_RUNTIME_KEY]={version:"${SEAMLESS_RUNTIME_VERSION}",port:PORT,stop:_qtStop}}catch(e){}
_qtLog("脚本已加载, 端口="+PORT);

function tryHookAuth(){
    if(window.__qtAuthService)return;
    if(window.__mcAuthService){window.__qtAuthService=window.__mcAuthService;_qtLog("从 __mcAuthService 获取 auth");return}
}

function _qtRuntimeHeartbeat(){
    if(_qtStopped)return;
    try{
        var focused=false;
        try{focused=!!document.hasFocus()}catch(e){}
        fetch("http://127.0.0.1:"+PORT+"/api/runtime-heartbeat?clientId="+encodeURIComponent(qtClientId||"")+"&windowScopeId="+encodeURIComponent(qtGetWindowScopeId())+"&focused="+(focused?"1":"0")+"&version="+encodeURIComponent("${SEAMLESS_RUNTIME_VERSION}"))
            .then(function(r){return r&&r.json?r.json():null})
            .then(function(d){
                var adoptedPort=d&&Number(d.batchPort)||0;
                var adoptedScope=d&&String(d.batchWorkspaceScopeId||"").trim();
                if(adoptedScope)qtSetBatchWorkspaceScopeId(adoptedScope,"heartbeat");
                if(adoptedPort&&adoptedPort!==_batchRetryPort){
                    _batchRetryPort=adoptedPort;
                    try{window.__qtBatchRetryPort=_batchRetryPort}catch(e){}
                    try{_cmdPollOk()}catch(e){}
                    _rLog("Batch retry bridge port adopted from heartbeat "+adoptedPort);
                }
            })
            .catch(function(){});
    }catch(e){}
}

var polling=false;
var _switchPollFailures=0;
var _switchPollBackoffUntil=0;
function _switchPollOk(){_switchPollFailures=0;_switchPollBackoffUntil=0}
function _switchPollFailed(){
    _switchPollFailures=Math.min(_switchPollFailures+1,6);
    _switchPollBackoffUntil=Date.now()+Math.min(30000,1000*Math.pow(2,_switchPollFailures-1));
}
function pollSwitch(){
    if(_qtStopped)return;
    if(polling)return;
    if(Date.now()<_switchPollBackoffUntil)return;
    if(!window.__qtAuthService)return;
    polling=true;
    fetch("http://127.0.0.1:"+PORT+"/api/pending-switch?clientId="+encodeURIComponent(qtClientId||""))
        .then(function(r){return r.json()})
        .then(function(d){
            polling=false;
            _switchPollOk();
            if(!d||!d.token)return;
            _qtLog("收到切换: "+d.email);
            var auth=window.__qtAuthService;
            if(!auth){_qtLog("auth 不存在");_postSwitchDone(d.id,false,"auth_missing");return}
            if(!("localOverrideAccessToken" in auth)){_qtLog("无 localOverrideAccessToken 属性");_postSwitchDone(d.id,false,"localOverrideAccessToken_missing");return}
            try{
                auth.localOverrideAccessToken=d.token;
                _qtLog("token 已设置");
                if(d.refreshToken){
                    auth.refreshToken=function(){return d.refreshToken};
                }
                if(!auth.__qtPatched){
                    auth.__qtPatched=true;
                    auth.storeAccessRefreshToken=function(r,s){
                        auth.localOverrideAccessToken=r;
                        if(s)auth.refreshToken=function(){return s};
                    };
                }
                _qtApplyMachineIds(auth,d.machineIds||d.machine_ids);
                _qtPatchEmailDisplay(d.email||"");
                var notifyError="";
                try{auth.notifyLoginChangedListeners(true);_qtLog("notifyLoginChangedListeners 已调用")}catch(e){notifyError=String(e&&e.message||e);_qtLog("notify 失败: "+notifyError)}
                if(auth.localOverrideAccessToken!==d.token){
                    _postSwitchDone(d.id,false,"token_set_mismatch");
                    return;
                }
                _postSwitchDone(d.id,true,notifyError?("notify_failed:"+notifyError):"");
            }catch(e){
                _postSwitchDone(d.id,false,String(e&&e.message||e));
            }
        })
        .catch(function(e){polling=false;_switchPollFailed();});
}

function _postSwitchDone(id,ok,error){
    try{
        fetch("http://127.0.0.1:"+PORT+"/api/switch-done?clientId="+encodeURIComponent(qtClientId||""),{
            method:"POST",
            headers:{"Content-Type":"application/json"},
            body:JSON.stringify({id:id||"",ok:!!ok,error:error||"",clientId:qtClientId||""})
        }).catch(function(){});
    }catch(e){}
}

function _qtApplyMachineIds(auth,machineIds){
    try{
        if(!auth||!machineIds)return;
        if(machineIds["telemetry.machineId"])auth._machineId=machineIds["telemetry.machineId"];
        if(machineIds["telemetry.macMachineId"])auth._macMachineId=machineIds["telemetry.macMachineId"];
        if(machineIds["telemetry.devDeviceId"])auth._devDeviceId=machineIds["telemetry.devDeviceId"];
        if(machineIds["storage.serviceMachineId"])auth._serviceMachineId=machineIds["storage.serviceMachineId"];
        _qtLog("machineIds 已同步到运行时 auth");
    }catch(e){_qtLog("machineIds 同步失败: "+String(e&&e.message||e))}
}

function _qtPatchEmailDisplay(email){
    try{
        if(!email)return;
        window.__qtCurrentEmail=email;
        var selectors=[
            ".cursor-settings-sidebar-header-email",
            "[class*=settings-sidebar-header-email]",
            "[class*=account] [class*=email]"
        ];
        for(var i=0;i<selectors.length;i++){
            var el=document.querySelector(selectors[i]);
            if(el&&el.textContent!==email)el.textContent=email;
        }
    }catch(e){}
}

function _qtWatchEmailDisplay(){
    try{
        if(window.__qtEmailObserver)return;
        window.__qtEmailObserver=new MutationObserver(function(){_qtPatchEmailDisplay(window.__qtCurrentEmail||"")});
        window.__qtEmailObserver.observe(document.body,{childList:true,subtree:true});
    }catch(e){}
}

_qtSetInterval(_qtRuntimeHeartbeat,2000);
_qtSetInterval(function(){tryHookAuth();pollSwitch()},POLL);
_qtSetTimeout(function(){tryHookAuth();_qtWatchEmailDisplay();_qtLog("auth 状态: "+(window.__qtAuthService?"已获取":"未获取"))},2000);
function qtVisible(el){
    try{
        if(!el)return false;
        var r=el.getBoundingClientRect();
        var s=getComputedStyle(el);
        return r.width>20&&r.height>10&&s.visibility!=="hidden"&&s.display!=="none"&&s.opacity!=="0";
    }catch(e){return false}
}
// 宽松版：anysphere-icon-button 是 20x20，上面的 >20 阈值会误杀它
function qtVisibleAny(el){
    try{
        if(!el)return false;
        var r=el.getBoundingClientRect();
        var s=getComputedStyle(el);
        return r.width>0&&r.height>0&&s.visibility!=="hidden"&&s.display!=="none"&&s.opacity!=="0";
    }catch(e){return false}
}
function qtScoreInput(el){
    var text=[
        el.getAttribute("aria-label")||"",
        el.getAttribute("placeholder")||"",
        el.getAttribute("data-testid")||"",
        el.getAttribute("class")||"",
        el.id||""
    ].join(" ").toLowerCase();
    var score=0;
    if(/composer|chat|agent|message|prompt|ask|input|textarea|aichat/.test(text))score+=20;
    if(/输入|发送|消息|提问|询问|聊天|开场/.test(text))score+=20;
    if(el.tagName==="TEXTAREA")score+=10;
    if(el.getAttribute("contenteditable")==="true")score+=8;
    var r=el.getBoundingClientRect();
    score+=Math.min(20,Math.floor((r.width*r.height)/5000));
    score+=Math.max(0,Math.floor((window.innerHeight-r.top)/200));
    return score;
}
function qtInputY(el){
    try{
        var r=el.getBoundingClientRect();
        return r.top+r.height/2;
    }catch(e){return 0}
}
function qtIsLikelyMessageEditor(el){
    if(!el||!el.closest)return false;
    return !!(
        el.closest(".composer-human-message")||
        el.closest(".composer-sticky-human-message")||
        el.closest('[class*="sticky-human-message" i]')||
        el.closest('[class*="human-message" i][class*="composer" i]')
    );
}
function qtFindComposerInput(){
    // 优先：document.activeElement（新建 Composer 后这里通常就是新输入框）
    // 避免多 Composer 下拿到 DOM 顺序靠前的旧窗口 input
    var ae=document.activeElement;
    if(ae&&qtVisible(ae)){
        var aeTag=ae.tagName;
        var aeType=((ae.getAttribute&&ae.getAttribute("type"))||"text").toLowerCase();
        if(aeTag==="TEXTAREA"||
           (aeTag==="INPUT"&&(aeType==="text"||aeType==="search"))||
           (ae.getAttribute&&ae.getAttribute("contenteditable")==="true")||
           (ae.getAttribute&&ae.getAttribute("role")==="textbox")){
            return ae;
        }
    }
    var list=Array.prototype.slice.call(document.querySelectorAll('textarea:not([disabled]), input[type="text"]:not([disabled]), [contenteditable="true"], [role="textbox"]'));
    list=list.filter(qtVisible).filter(function(el){
        var tag=el.tagName;
        if(tag==="INPUT"){
            var type=(el.getAttribute("type")||"text").toLowerCase();
            if(type!=="text"&&type!=="search")return false;
        }
        return true;
    });
    list.sort(function(a,b){return qtScoreInput(b)-qtScoreInput(a)});
    return list[0]||null;
}
function qtComposerRootFromInput(input){
    if(!input||!input.closest)return null;
    return input.closest('[data-composer-id]')||
        input.closest('[data-testid*="composer" i]')||
        input.closest('[class*="composer" i]')||
        null;
}
function qtComposerIdFromRoot(root){
    if(!root)return "";
    return String(root.getAttribute("data-composer-id")||
        root.getAttribute("data-composerid")||
        root.getAttribute("data-id")||
        "");
}
function qtComposerIdFromInput(input){
    return qtComposerIdFromRoot(qtComposerRootFromInput(input));
}
function qtFindComposerRootById(id){
    id=String(id||"").trim();
    if(!id)return null;
    var escaped=id.replace(/["\\\\]/g,"\\\\$&");
    try{
        var direct=document.querySelector('[data-composer-id="'+escaped+'"]');
        if(direct&&qtVisibleAny(direct))return direct;
    }catch(e){}
    var roots=Array.prototype.slice.call(document.querySelectorAll('[data-composer-id]'));
    for(var i=0;i<roots.length;i++){
        if(qtComposerIdFromRoot(roots[i])===id&&qtVisibleAny(roots[i]))return roots[i];
    }
    return null;
}
function qtFindInputInComposerRoot(root){
    if(!root)return null;
    var list=Array.prototype.slice.call(root.querySelectorAll('textarea:not([disabled]), input[type="text"]:not([disabled]), [contenteditable="true"], [role="textbox"]'));
    list=list.filter(qtVisible).filter(function(el){
        var tag=el.tagName;
        if(tag==="INPUT"){
            var type=(el.getAttribute("type")||"text").toLowerCase();
            if(type!=="text"&&type!=="search")return false;
        }
        return true;
    });
    list.sort(function(a,b){
        var ae=qtIsLikelyMessageEditor(a);
        var be=qtIsLikelyMessageEditor(b);
        if(ae!==be)return ae?1:-1;
        var dy=qtInputY(b)-qtInputY(a);
        if(Math.abs(dy)>80)return dy;
        return qtScoreInput(b)-qtScoreInput(a);
    });
    return list[0]||null;
}
var qtWindowScopeId="";
var qtBatchWorkspaceScopeId="";
function qtMakeScopeId(prefix){
    return String(prefix||"qts")+"-"+Date.now().toString(36)+"-"+Math.random().toString(36).slice(2,10);
}
function qtInitWindowScopeId(){
    try{
        var key="__qingtian_window_scope_id";
        var existing=sessionStorage.getItem(key);
        if(existing)return String(existing);
        var id=qtMakeScopeId("qtw");
        sessionStorage.setItem(key,id);
        return id;
    }catch(e){
        try{
            if(window.__qingtian_window_scope_id)return String(window.__qingtian_window_scope_id);
            window.__qingtian_window_scope_id=qtMakeScopeId("qtw");
            return String(window.__qingtian_window_scope_id);
        }catch(e2){
            return qtMakeScopeId("qtw");
        }
    }
}
function qtGetWindowScopeId(){
    if(!qtWindowScopeId)qtWindowScopeId=qtInitWindowScopeId();
    return String(qtWindowScopeId||"");
}
function qtSetBatchWorkspaceScopeId(scopeId,source){
    scopeId=String(scopeId||"").trim();
    if(!scopeId)return;
    if(qtBatchWorkspaceScopeId===scopeId)return;
    qtBatchWorkspaceScopeId=scopeId;
    try{window.__qtBatchWorkspaceScopeId=scopeId}catch(e){}
    qtReleaseHookLog("workspace_scope_set scope="+scopeId+" source="+(source||"unknown"));
    qtCleanupAutoPersistedChannelComposerBindings("workspace_scope_set");
}
function qtBindingScopeId(){
    var workspaceScope=String(qtBatchWorkspaceScopeId||"workspace-unknown").trim()||"workspace-unknown";
    var windowScope=qtGetWindowScopeId()||"window-unknown";
    return workspaceScope+"_"+windowScope;
}
function qtStoragePrefix(){return "__qingtian_channel_composer_v2_"+qtBindingScopeId()+"_"}
function qtStorageKey(channelId){return qtStoragePrefix()+String(channelId||"").trim()}
function qtParseChannelBindingStorageKey(key){
    key=String(key||"");
    var prefix=qtStoragePrefix();
    if(key.indexOf(prefix)!==0)return "";
    return key.slice(prefix.length);
}
var qtRuntimeComposerChannelMap={};
var qtRuntimeChannelComposerMap={};
var qtStopHookProbeState={};
function qtReleaseHookLog(message){
    try{
        var text=String(message||"");
        if(typeof _rLog==="function")_rLog("release_hook "+text);
        else if(typeof _bLog==="function")_bLog("release_hook "+text);
        else _qtLog("release_hook "+text);
    }catch(e){}
}
function qtStopHookProbeLog(key,state){
    try{
        key=String(key||"");
        state=String(state||"");
        if(!key||!state)return;
        if(qtStopHookProbeState[key]===state)return;
        qtStopHookProbeState[key]=state;
        qtReleaseHookLog("hook_probe key="+key+" state="+state);
    }catch(e){}
}
function qtRememberChannelComposer(channelId,composerId,source){
    channelId=String(channelId||"").trim();
    composerId=String(composerId||"").trim();
    if(!channelId||!composerId)return;
    var item={channelId:channelId,composerId:composerId,source:String(source||"runtime"),updatedAt:Date.now(),clientId:qtClientId||"",windowScopeId:qtGetWindowScopeId(),workspaceScopeId:qtBatchWorkspaceScopeId||"",bindingScopeId:qtBindingScopeId()};
    qtRuntimeComposerChannelMap[composerId]=item;
    qtRuntimeChannelComposerMap[channelId]=item;
    qtReleaseHookLog("runtime_map_set channel="+channelId+" composer="+composerId+" source="+item.source);
}
function qtShouldPersistChannelComposerBinding(source){
    source=String(source||"").trim();
    return source==="manual";
}
function qtIsAutoPersistedChannelBindingSource(source){
    source=String(source||"").trim();
    return source==="auto_prompt_submit"||
        source==="auto_reuse_existing"||
        source==="auto_silent_launch"||
        source==="check_messages_scan"||
        source==="running_check_messages_scan";
}
function qtCleanupAutoPersistedChannelComposerBindings(reason){
    var removed=0;
    try{
        var keys=[];
        for(var i=0;i<localStorage.length;i++){
            var key=localStorage.key(i)||"";
            if(qtParseChannelBindingStorageKey(key))keys.push(key);
        }
        for(var j=0;j<keys.length;j++){
            var raw=localStorage.getItem(keys[j]);
            var data=raw?JSON.parse(raw):{};
            if(!qtIsAutoPersistedChannelBindingSource(data&&data.source))continue;
            localStorage.removeItem(keys[j]);
            removed++;
        }
        if(removed>0)qtReleaseHookLog("cleanup_auto_persistent_bindings removed="+removed+" reason="+(reason||"startup")+" scope="+qtBindingScopeId());
    }catch(e){
        qtReleaseHookLog("cleanup_auto_persistent_bindings_failed reason="+(reason||"startup")+" error="+String(e&&e.message||e));
    }
    return removed;
}
function qtPersistChannelComposerBinding(channelId,composerId,source){
    channelId=String(channelId||"").trim();
    composerId=String(composerId||"").trim();
    if(!channelId||!composerId)return false;
    try{
        var record={channelId:channelId,composerId:composerId,clientId:qtClientId||"",windowScopeId:qtGetWindowScopeId(),workspaceScopeId:qtBatchWorkspaceScopeId||"",bindingScopeId:qtBindingScopeId(),source:String(source||"unknown"),updatedAt:Date.now()};
        localStorage.setItem(qtStorageKey(channelId),JSON.stringify(record));
        qtReleaseHookLog("persistent_binding_set channel="+channelId+" composer="+composerId+" client="+(qtClientId||"")+" source="+record.source+" scope="+record.bindingScopeId);
        return true;
    }catch(e){return false}
}
function qtBindChannelComposer(channelId,composerId,source){
    channelId=String(channelId||"").trim();
    composerId=String(composerId||"").trim();
    if(!channelId||!composerId)return;
    qtRememberChannelComposer(channelId,composerId,source||"unknown");
    if(!qtShouldPersistChannelComposerBinding(source)){
        qtReleaseHookLog("skip_persistent_auto_binding channel="+channelId+" composer="+composerId+" source="+(source||"auto"));
        return;
    }
    if(qtPersistChannelComposerBinding(channelId,composerId,source||"unknown")){
        _qtLog("prompt: 绑定 CH-"+channelId+" -> composer "+composerId+" client="+(qtClientId||""));
    }
}
function qtReadBoundComposerId(channelId){
    try{
        var raw=localStorage.getItem(qtStorageKey(channelId));
        if(!raw)return "";
        var data=JSON.parse(raw);
        return String(data&&data.composerId||"").trim();
    }catch(e){return ""}
}
function qtClearChannelComposerBinding(channelId,composerId){
    channelId=String(channelId||"").trim();
    if(!channelId)return false;
    try{
        var raw=localStorage.getItem(qtStorageKey(channelId));
        if(!raw)return false;
        var data=JSON.parse(raw);
        if(composerId&&String(data&&data.composerId||"")!==String(composerId))return false;
        localStorage.removeItem(qtStorageKey(channelId));
        _qtLog("prompt: 清理失效绑定 CH-"+channelId+" -> "+(composerId||""));
        return true;
    }catch(e){return false}
}
function qtReadChannelComposerBindings(){
    var result=[];
    try{
        qtCleanupAutoPersistedChannelComposerBindings("read_bindings");
        for(var i=0;i<localStorage.length;i++){
            var key=localStorage.key(i)||"";
            var channelId=qtParseChannelBindingStorageKey(key);
            if(!channelId)continue;
            var raw=localStorage.getItem(key);
            var data=raw?JSON.parse(raw):{};
            var composerId=String(data&&data.composerId||"").trim();
            if(!channelId||!composerId)continue;
            result.push({channelId:String(channelId),composerId:composerId,clientId:String(data&&data.clientId||"").trim(),windowScopeId:String(data&&data.windowScopeId||"").trim(),workspaceScopeId:String(data&&data.workspaceScopeId||"").trim(),bindingScopeId:String(data&&data.bindingScopeId||"").trim(),source:String(data&&data.source||"").trim(),updatedAt:Number(data.updatedAt||0)});
        }
    }catch(e){}
    return result;
}
function qtFindRunningCheckMessagesChannel(composerId){
    composerId=String(composerId||"").trim();
    var root=composerId?qtFindComposerRootById(composerId):null;
    var selectors=[
        '[data-message-kind="tool"]',
        '[data-tool-status]',
        '[data-mcp-tool-status]',
        '.composer-tool-call-content',
        '.composer-tool-call-container',
        '.composer-mcp-tool-call-block'
    ].join(",");
    function collect(scope){
        try{return Array.prototype.slice.call((scope||document).querySelectorAll(selectors)).filter(qtVisibleAny)}catch(e){return []}
    }
    var blocks=[];
    if(root)blocks=blocks.concat(collect(root));
    if(!composerId)blocks=blocks.concat(collect(document));
    var best=null;
    for(var i=0;i<blocks.length;i++){
        var block=blocks[i];
        var text=qtToolBlockText(block);
        var match=String(text||"").match(/(Running|Ran|Cancelled|Canceled|Failed|Errored)\s*Check\s*Messages\s*in\s*qtwx-mcp-(\d+)/i);
        if(!match)match=String(text||"").match(/qtwx-mcp-(\d+)/i);
        if(!match)continue;
        var channelId=String(match[2]||match[1]||"").trim();
        if(!/^\d+$/.test(channelId))continue;
        var status=match[2]?String(match[1]||""):"unknown";
        var running=/^Running$/i.test(status)||/Running\s*Check\s*Messages/i.test(String(text||""));
        if(composerId){
            var blockComposerId="";
            try{blockComposerId=qtComposerIdFromRoot(block&&block.closest?block.closest("[data-composer-id]"):null)}catch(e){blockComposerId=""}
            if(blockComposerId&&blockComposerId!==composerId)continue;
        }
        var item={
            channelId:channelId,
            status:status,
            running:running,
            source:root&&root.contains&&root.contains(block)?"root":"global",
            text:String(text||"").replace(/\s+/g," ").slice(0,220)
        };
        if(running)return item;
        if(!best)best=item;
    }
    return best;
}
function qtListRunningCheckMessagesChannels(){
    var selectors=[
        '[data-message-kind="tool"]',
        '[data-tool-status]',
        '[data-mcp-tool-status]',
        '.composer-tool-call-content',
        '.composer-tool-call-container',
        '.composer-mcp-tool-call-block'
    ].join(",");
    function collect(scope){
        try{return Array.prototype.slice.call((scope||document).querySelectorAll(selectors)).filter(qtVisibleAny)}catch(e){return []}
    }
    function resolveComposerId(block,channelId){
        var root=null;
        try{root=block&&block.closest?block.closest("[data-composer-id]"):null}catch(e){root=null}
        var composerId=root?qtComposerIdFromRoot(root):"";
        return {composerId:composerId,root:root};
    }
    var blocks=collect(document);
    var byChannel={};
    for(var i=0;i<blocks.length;i++){
        var block=blocks[i];
        var text=qtToolBlockText(block);
        var match=String(text||"").match(/(Running|Ran|Cancelled|Canceled|Failed|Errored)\s*Check\s*Messages\s*in\s*qtwx-mcp-(\d+)/i);
        if(!match)continue;
        var status=String(match[1]||"").trim();
        var channelId=String(match[2]||"").trim();
        if(!/^\d+$/.test(channelId))continue;
        var attrStatus="";
        try{attrStatus=String(block.getAttribute("data-tool-status")||block.getAttribute("data-mcp-tool-status")||"")}catch(e){}
        if(!status&&attrStatus==="loading")status="Running";
        var running=/^Running$/i.test(status)||/Running\s*Check\s*Messages/i.test(String(text||""));
        if(!running)continue;
        var resolved=resolveComposerId(block,channelId);
        var composerId=String(resolved.composerId||"").trim();
        if(composerId)qtBindChannelComposer(channelId,composerId,"running_check_messages_scan");
        var root=resolved.root||qtFindComposerRootById(composerId);
        var activeRoot=document.activeElement&&document.activeElement.closest?document.activeElement.closest("[data-composer-id]"):null;
        var activeComposerId=activeRoot?qtComposerIdFromRoot(activeRoot):"";
        var score=100;
        if(composerId)score+=30;
        if(root)score+=20;
        if(attrStatus==="loading")score+=10;
        try{
            var r=block.getBoundingClientRect();
            score+=Math.max(0,1000-Math.round(r.top))/100;
        }catch(e){}
        var item={
            channelId:channelId,
            composerId:composerId,
            clientId:qtClientId||"",
            updatedAt:Date.now(),
            exists:true,
            loaded:true,
            attached:!!root,
            observable:true,
            clientMatches:true,
            activeComposerMatches:!!composerId&&activeComposerId===composerId,
            activeComposerId:activeComposerId,
            runningCheckActive:true,
            mcpWaitingActive:true,
            visibleGeneratingActive:false,
            connectedActive:true,
            runtimeOccupancyOnly:true,
            connectionReason:"running_check_messages_scan",
            checkMessagesStatus:status||"Running",
            checkMessagesEvidence:"running_check_messages:qtwx-mcp-"+channelId,
            checkMessagesSource:root&&root.contains&&root.contains(block)?"root":"global",
            text:String(text||"").replace(/\s+/g," ").slice(0,220),
            toolStatus:attrStatus,
            score:score
        };
        if(!byChannel[channelId]||item.score>byChannel[channelId].score)byChannel[channelId]=item;
    }
    var result=[];
    for(var key in byChannel){
        if(Object.prototype.hasOwnProperty.call(byChannel,key))result.push(byChannel[key]);
    }
    return result;
}
function qtListCheckMessagesChannelComposerBindings(){
    var selectors=[
        '[data-message-kind="tool"]',
        '[data-tool-status]',
        '[data-mcp-tool-status]',
        '.composer-tool-call-content',
        '.composer-tool-call-container',
        '.composer-mcp-tool-call-block'
    ].join(",");
    function collect(scope){
        try{return Array.prototype.slice.call((scope||document).querySelectorAll(selectors)).filter(qtVisibleAny)}catch(e){return []}
    }
    function resolveComposerId(block,channelId){
        var root=null;
        try{root=block&&block.closest?block.closest("[data-composer-id]"):null}catch(e){root=null}
        var composerId=root?qtComposerIdFromRoot(root):"";
        var source=composerId?"root":"";
        return {composerId:composerId,root:root,source:source};
    }
    var blocks=collect(document);
    var byChannel={};
    for(var i=0;i<blocks.length;i++){
        var block=blocks[i];
        var text=qtToolBlockText(block);
        var match=String(text||"").match(/(Running|Ran|Cancelled|Canceled|Failed|Errored)\s*Check\s*Messages\s*in\s*qtwx-mcp-(\d+)/i);
        if(!match)continue;
        var status=String(match[1]||"").trim();
        var channelId=String(match[2]||"").trim();
        if(!/^\d+$/.test(channelId))continue;
        var attrStatus="";
        try{attrStatus=String(block.getAttribute("data-tool-status")||block.getAttribute("data-mcp-tool-status")||"")}catch(e){}
        if(!status&&attrStatus==="loading")status="Running";
        var running=/^Running$/i.test(status)||/Running\s*Check\s*Messages/i.test(String(text||""));
        var resolved=resolveComposerId(block,channelId);
        var composerId=String(resolved.composerId||"").trim();
        if(!composerId)continue;
        var root=resolved.root||qtFindComposerRootById(composerId);
        var activeRoot=document.activeElement&&document.activeElement.closest?document.activeElement.closest("[data-composer-id]"):null;
        var activeComposerId=activeRoot?qtComposerIdFromRoot(activeRoot):"";
        var persistSource=running?"running_check_messages_scan":"check_messages_scan";
        qtBindChannelComposer(channelId,composerId,persistSource);
        var score=running?200:100;
        if(resolved.source==="root")score+=40;
        if(root)score+=20;
        if(attrStatus==="loading")score+=10;
        try{
            var r=block.getBoundingClientRect();
            score+=Math.max(0,1000-Math.round(r.top))/100;
        }catch(e){}
        var item={
            channelId:channelId,
            composerId:composerId,
            clientId:qtClientId||"",
            updatedAt:Date.now(),
            exists:true,
            loaded:true,
            attached:!!root,
            observable:true,
            clientMatches:true,
            activeComposerMatches:!!composerId&&activeComposerId===composerId,
            activeComposerId:activeComposerId,
            runningCheckActive:running,
            mcpWaitingActive:running,
            visibleGeneratingActive:false,
            connectedActive:running,
            runtimeOccupancyOnly:false,
            currentMcpSession:true,
            source:persistSource,
            connectionReason:persistSource,
            checkMessagesStatus:status||"",
            checkMessagesEvidence:(running?"running_check_messages:":"check_messages_status:")+"qtwx-mcp-"+channelId+":"+status,
            checkMessagesSource:resolved.source||"",
            text:String(text||"").replace(/\s+/g," ").slice(0,220),
            toolStatus:attrStatus,
            score:score
        };
        if(!byChannel[channelId]||item.score>byChannel[channelId].score)byChannel[channelId]=item;
    }
    var result=[];
    for(var key in byChannel){
        if(Object.prototype.hasOwnProperty.call(byChannel,key))result.push(byChannel[key]);
    }
    return result;
}
function qtResolveChannelIdByComposerIdDetails(composerId){
    composerId=String(composerId||"").trim();
    if(!composerId)return {channelId:"",source:"missing_composer",evidence:null};
    var runtime=qtRuntimeComposerChannelMap[composerId];
    if(runtime&&runtime.channelId)return {channelId:String(runtime.channelId||""),source:"runtime_map",evidence:runtime};
    var bindings=qtReadChannelComposerBindings();
    for(var i=0;i<bindings.length;i++){
        if(String(bindings[i]&&bindings[i].composerId||"").trim()===composerId){
            return {channelId:String(bindings[i].channelId||"").trim(),source:"persistent_binding",evidence:bindings[i]};
        }
    }
    var running=qtFindRunningCheckMessagesChannel(composerId);
    if(running&&running.channelId)return {channelId:String(running.channelId||""),source:"tool_block",evidence:running};
    return {channelId:"",source:"not_found",evidence:null};
}
function qtResolveChannelIdByComposerId(composerId){
    return qtResolveChannelIdByComposerIdDetails(composerId).channelId||"";
}
function qtIsImplicitComposerStopReason(reason){
    reason=String(reason||"").trim();
    return /^cursor_/.test(reason)||reason==="bridge_stop_by_composer";
}
function qtFindBatchRetryInstanceForRelease(channelId,composerId){
    channelId=String(channelId||"").trim();
    composerId=String(composerId||"").trim();
    try{
        if(typeof _instances!=="object"||!_instances)return null;
        if(composerId&&_instances[composerId])return _instances[composerId];
        for(var cid in _instances){
            if(!Object.prototype.hasOwnProperty.call(_instances,cid))continue;
            var inst=_instances[cid];
            if(!inst)continue;
            if(composerId&&String(inst.composerId||cid||"").trim()===composerId)return inst;
            if(channelId&&String(inst.channelId||"").trim()===channelId)return inst;
        }
    }catch(e){}
    return null;
}
function qtShouldPreserveBatchMcpWaitingRelease(channelId,composerId,reason){
    if(!qtIsImplicitComposerStopReason(reason))return false;
    var inst=qtFindBatchRetryInstanceForRelease(channelId,composerId);
    if(!inst){
        qtReleaseHookLog("preserve_check_no_instance reason="+reason+" channel="+(channelId||"")+" composer="+(composerId||""));
        return false;
    }
    channelId=String(channelId||inst.channelId||"").trim();
    composerId=String(composerId||inst.composerId||"").trim();
    qtReleaseHookLog("preserve_check_instance reason="+reason+" channel="+(channelId||"")+" composer="+(composerId||"")+" running="+(!!inst.running)+" task="+(inst.taskId||"")+" generation="+(inst.generation||"")+" attempts="+(inst.attempts||0));
    try{
        var bridge=window.__qtComposerBridge;
        if(bridge&&bridge.getStatus&&composerId){
            var status=bridge.getStatus(composerId,channelId);
            var evidence=status&&status.runningCheckMessages;
            qtReleaseHookLog("preserve_check_status reason="+reason+" channel="+(channelId||"")+" composer="+(composerId||"")+" status="+(status&&status.status||"")+" busy="+(status&&status.busy||"")+" runningCheck="+(evidence&&evidence.ok?1:0)+" evidenceChannel="+(evidence&&evidence.channelId||""));
            if(evidence&&evidence.ok&&(!channelId||String(evidence.channelId||"").trim()===channelId)){
                qtReleaseHookLog("preserve_mcp_waiting_release reason="+reason+" channel="+(channelId||"")+" composer="+(composerId||"")+" source=status");
                return true;
            }
        }
    }catch(e){}
    try{
        var running=qtFindRunningCheckMessagesChannel(composerId);
        qtReleaseHookLog("preserve_check_tool_block reason="+reason+" channel="+(channelId||"")+" composer="+(composerId||"")+" running="+(running&&running.running?1:0)+" blockChannel="+(running&&running.channelId||"")+" status="+(running&&running.status||"")+" source="+(running&&running.source||""));
        if(running&&running.running&&(!channelId||String(running.channelId||"").trim()===channelId)){
            qtReleaseHookLog("preserve_mcp_waiting_release reason="+reason+" channel="+(channelId||"")+" composer="+(composerId||"")+" source=tool_block");
            return true;
        }
    }catch(e){}
    return false;
}
function qtReleaseChannelWaiting(channelId,composerId,reason,port){
    channelId=String(channelId||"").trim();
    composerId=String(composerId||"").trim();
    reason=String(reason||"client_stop").trim()||"client_stop";
    var resolveSource=channelId?"provided":"";
    if(!channelId&&composerId){
        var resolved=qtResolveChannelIdByComposerIdDetails(composerId);
        channelId=resolved.channelId||"";
        resolveSource=resolved.source||"";
        qtReleaseHookLog("resolve_channel composer="+composerId+" channel="+(channelId||"")+" source="+resolveSource+" reason="+reason);
    }
    if(!channelId&&!composerId){
        qtReleaseHookLog("release_skipped_missing_ids reason="+reason);
        return false;
    }
    if(qtShouldPreserveBatchMcpWaitingRelease(channelId,composerId,reason)){
        qtReleaseHookLog("release_skipped_preserve_mcp_waiting channel="+(channelId||"")+" composer="+(composerId||"")+" reason="+reason);
        return false;
    }
    try{
        port=Number(port)||26399;
        var diagId="rel-"+Date.now().toString(36)+"-"+Math.random().toString(36).slice(2,7);
        var xhr=new XMLHttpRequest();
        xhr.open("POST","http://127.0.0.1:"+port+"/auto-chat/release-channel",true);
        xhr.setRequestHeader("Content-Type","application/json");
        xhr.timeout=1200;
        xhr.onload=function(){qtReleaseHookLog("release_xhr_done diagId="+diagId+" status="+xhr.status+" channel="+(channelId||"")+" composer="+(composerId||"")+" source="+resolveSource+" response="+String(xhr.responseText||"").slice(0,240))};
        xhr.onerror=function(){qtReleaseHookLog("release_xhr_error diagId="+diagId+" channel="+(channelId||"")+" composer="+(composerId||"")+" source="+resolveSource+" port="+port)};
        xhr.ontimeout=function(){qtReleaseHookLog("release_xhr_timeout diagId="+diagId+" channel="+(channelId||"")+" composer="+(composerId||"")+" source="+resolveSource+" port="+port)};
        xhr.send(JSON.stringify({channelId:channelId,composerId:composerId,reason:reason,clientId:qtClientId||"",diagId:diagId}));
        qtReleaseHookLog("release_sent diagId="+diagId+" channel="+(channelId||"")+" composer="+(composerId||"")+" reason="+reason+" source="+resolveSource+" port="+port);
        return true;
    }catch(e){
        qtReleaseHookLog("release_exception channel="+(channelId||"")+" composer="+(composerId||"")+" error="+String(e&&e.message||e));
        return false;
    }
}
function qtSetInputValue(el,text){
    el.focus();
    if(el.tagName==="TEXTAREA"||el.tagName==="INPUT"){
        var proto=el.tagName==="TEXTAREA"?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;
        var desc=Object.getOwnPropertyDescriptor(proto,"value");
        if(desc&&desc.set)desc.set.call(el,text);else el.value=text;
        el.dispatchEvent(new Event("input",{bubbles:true}));
        el.dispatchEvent(new Event("change",{bubbles:true}));
        try{el.setSelectionRange(text.length,text.length)}catch(e){}
        return;
    }
    // ProseMirror / contenteditable —— 只用 execCommand("insertText") 一次
    // —— execCommand 自己会触发原生 beforeinput / input 事件，ProseMirror 消化一次即可
    // —— 绝对不要再手动 dispatchEvent("beforeinput")/dispatchEvent("input")，
    //    否则 ProseMirror 会把同一段文本消化三遍（经诊断验证）
    var ok=false;
    try{
        var sel=window.getSelection();
        var range=document.createRange();
        range.selectNodeContents(el);
        range.deleteContents();
        sel.removeAllRanges();
        sel.addRange(range);
        ok=document.execCommand("insertText",false,text);
    }catch(e){ok=false}
    if(ok)return;
    // 兜底 1：合成 beforeinput（某些 ProseMirror 版本会响应同步写入）
    try{
        var ev=new InputEvent("beforeinput",{bubbles:true,cancelable:true,inputType:"insertText",data:text});
        el.dispatchEvent(ev);
        if((el.innerText||el.textContent||"").indexOf(String(text))>=0)return;
    }catch(e){}
    // 兜底 2：直接写 textContent
    try{el.textContent=text}catch(e){}
}
// 发送按钮查找：先以 input 为锚点查找同一 Composer 实例的按钮，避免跨窗口拿错
function qtFindSendButtonNearInput(input){
    if(!input)return null;
    // 策略 1：沿 input 祖先树查找 send-with-mode，命中后下钻到真正可点击元素
    // Cursor 3.2 的结构：<div class="send-with-mode"><div class="anysphere-icon-button" data-mode="agent">…</div></div>
    var p=input.parentElement,depth=0;
    while(p&&p!==document.body&&depth<12){
        var wraps=p.querySelectorAll('[class*="send-with-mode" i]');
        for(var i=0;i<wraps.length;i++){
            var wrap=wraps[i];
            if(!qtVisibleAny(wrap))continue;
            var inner=wrap.querySelector('button,[role="button"],[class*="submit" i],[class*="anysphere-icon-button" i],[data-mode]');
            if(inner&&qtVisibleAny(inner))return inner;
            return wrap;
        }
        p=p.parentElement;depth++;
    }
    // 策略 2：几何邻近（input 周边的 send-with-mode）
    var ir=input.getBoundingClientRect();
    var allWraps=document.querySelectorAll('[class*="send-with-mode" i]');
    var closest=null,closestDist=Infinity;
    for(var j=0;j<allWraps.length;j++){
        var w=allWraps[j];
        if(!qtVisibleAny(w))continue;
        var wr=w.getBoundingClientRect();
        var dy=wr.top-ir.top;
        if(dy<-50||dy>400)continue;
        var dist=Math.abs(wr.left-ir.right)+Math.abs(dy);
        if(dist<closestDist){closest=w;closestDist=dist}
    }
    if(closest){
        var inner2=closest.querySelector('button,[role="button"],[class*="submit" i],[class*="anysphere-icon-button" i],[data-mode]');
        if(inner2&&qtVisibleAny(inner2))return inner2;
        return closest;
    }
    // 策略 3：退让到评分查找
    return qtFindSendButton(input);
}
function qtFindSendButton(input){
    // Tier 1：严格 <button> + 精准 selector
    var directBtn=null;
    var directSels=[
        'button[data-testid*="composer-submit" i]',
        'button[data-testid*="send" i]',
        'button[class*="send-with-mode" i]',
        'button[aria-label*="send" i]:not([aria-label*="image" i]):not([aria-label*="file" i])',
        'button[aria-label*="submit" i]'
    ];
    for(var k=0;k<directSels.length;k++){
        try{var b=document.querySelector(directSels[k]);if(b&&qtVisible(b)){directBtn=b;break}}catch(e){}
    }
    if(directBtn)return directBtn;
    // Tier 2：role="button" + send-with-mode
    try{
        var rb=document.querySelector('[role="button"][class*="send-with-mode" i]');
        if(rb&&qtVisibleAny(rb))return rb;
    }catch(e){}
    // Tier 3：send-with-mode 外壳下钻 anysphere-icon-button（Cursor 3.2 发送按钮内核）
    var wraps=document.querySelectorAll('[class*="send-with-mode" i]');
    for(var ii=0;ii<wraps.length;ii++){
        var ww=wraps[ii];
        if(!qtVisibleAny(ww))continue;
        var inn=ww.querySelector('button,[role="button"],[class*="submit" i],[class*="anysphere-icon-button" i],[data-mode]');
        if(inn&&qtVisibleAny(inn))return inn;
    }
    // Tier 4：通过 codicon-arrow-up-two 图标反找其 closest 可点击容器
    var arrowUps=document.querySelectorAll('.codicon-arrow-up-two');
    for(var jj=0;jj<arrowUps.length;jj++){
        var ic=arrowUps[jj].closest('[class*="anysphere-icon-button" i],[role="button"],button');
        if(ic&&qtVisibleAny(ic))return ic;
    }
    // Tier 5：send-with-mode 外壳自身
    for(var kk=0;kk<wraps.length;kk++){if(qtVisibleAny(wraps[kk]))return wraps[kk]}

    // Tier 6：退回原评分逻辑（兼容老版本 Cursor）
    var roots=[];
    var cur=input;
    for(var i=0;i<7&&cur;i++,cur=cur.parentElement)roots.push(cur);
    roots.push(document.body);
    var ir=input.getBoundingClientRect();
    var best=null;
    for(var r=0;r<roots.length;r++){
        var buttons=Array.prototype.slice.call(roots[r].querySelectorAll('button,[role="button"],a.action-label,.codicon-send,.codicon-arrow-up,.codicon-arrow-circle-up,[class*="send"],[class*="submit"]')).filter(qtVisible);
        buttons.forEach(function(btn){
            var text=[
                btn.innerText||"",
                btn.getAttribute("aria-label")||"",
                btn.getAttribute("title")||"",
                btn.getAttribute("data-testid")||"",
                btn.getAttribute("class")||""
            ].join(" ").toLowerCase();
            if(/new agent|replace agent|history|settings|statusbar|titlebar|toggle agents|close|notification/.test(text))return;
            if(/image|attach|file|upload|mic|voice|context|model|photo|picture/.test(text))return;
            var score=0;
            if(/send|submit|run|agent|arrow|paper|composer/.test(text))score+=40;
            if(btn.disabled||btn.getAttribute("aria-disabled")==="true")score-=100;
            var br=btn.getBoundingClientRect();
            var dxRight=Math.abs(br.right-ir.right);
            var dyBottom=Math.abs(br.bottom-ir.bottom);
            if(br.left>ir.left+ir.width*0.55)score+=20;
            if(br.top>ir.top+ir.height*0.45)score+=20;
            if(br.top<ir.top-20)score-=120;
            if(br.bottom<ir.top||br.top>ir.bottom+160)score-=80;
            if(br.right<ir.left||br.left>ir.right+180)score-=80;
            score+=Math.max(0,40-dxRight/5);
            score+=Math.max(0,30-dyBottom/4);
            if(!best||score>best.score)best={btn:btn,score:score};
        });
    }
    return best&&best.score>15?best.btn:null;
}
function qtPressEnter(el){
    ["keydown","keypress","keyup"].forEach(function(type){
        el.dispatchEvent(new KeyboardEvent(type,{key:"Enter",code:"Enter",which:13,keyCode:13,bubbles:true,cancelable:true}));
    });
}
function qtReadInputText(el){
    if(!el)return "";
    if("value" in el)return String(el.value||"");
    return String(el.innerText||el.textContent||"");
}
function qtNormalizeText(text){
    return String(text||"").replace(/\s+/g," ").trim();
}
function qtTextHash(text){
    text=qtNormalizeText(text);
    var h=0;
    for(var i=0;i<text.length;i++){
        h=((h*31)+text.charCodeAt(i))|0;
    }
    return String(h);
}
function qtEscapeRegExp(text){
    return String(text||"").replace(/[.*+?^\${}()|[\]\\]/g,"\\$&");
}
function qtToolBlockText(el){
    if(!el)return "";
    var parts=[];
    try{if(el.innerText)parts.push(el.innerText)}catch(e){}
    try{if(el.textContent)parts.push(el.textContent)}catch(e){}
    try{if(el.getAttribute){
        ["aria-label","title","data-tooltip"].forEach(function(name){
            var value=el.getAttribute(name);
            if(value)parts.push(value);
        });
    }}catch(e){}
    return qtNormalizeText(parts.join(" "));
}
function qtRunningCheckEvidence(block,channelId,toolName,text,source){
    return {
        ok:true,
        channelId:channelId,
        toolName:toolName,
        evidence:"running_check_messages:"+toolName,
        source:source||"",
        text:String(text||"").slice(0,800),
        tag:block&&block.tagName||"",
        className:block?String(block.className||"").slice(0,240):"",
        messageKind:block&&block.getAttribute&&block.getAttribute("data-message-kind")||"",
        toolStatus:block&&block.getAttribute&&(block.getAttribute("data-tool-status")||block.getAttribute("data-mcp-tool-status"))||""
    };
}
function qtReadBottomComposerButtonState(){
    var result={hasStopButton:false,hasMicButton:false,stopRect:"",micRect:""};
    function inBottomRight(el){
        try{
            if(!qtVisibleAny(el))return false;
            var r=el.getBoundingClientRect();
            return r.width>0&&r.width<=80&&r.height>0&&r.height<=80&&
                r.bottom>window.innerHeight*0.65&&r.right>window.innerWidth*0.45&&
                r.left>=0&&r.top>=0&&r.right<=window.innerWidth+8&&r.bottom<=window.innerHeight+8;
        }catch(e){return false}
    }
    function rectText(el){
        try{
            var r=el.getBoundingClientRect();
            return [Math.round(r.left),Math.round(r.top),Math.round(r.width),Math.round(r.height)].join(",");
        }catch(e){return ""}
    }
    var stop=[];
    try{stop=Array.prototype.slice.call(document.querySelectorAll('[data-stop-button="true"], .codicon-debug-stop, .codicon-stop')).filter(inBottomRight)}catch(e){stop=[]}
    if(stop.length){
        result.hasStopButton=true;
        result.stopRect=rectText(stop[0]);
    }
    var mic=[];
    try{mic=Array.prototype.slice.call(document.querySelectorAll('.codicon-mic, [class*="mic-icon" i], [aria-label*="mic" i], [aria-label*="microphone" i]')).filter(inBottomRight)}catch(e){mic=[]}
    if(mic.length){
        result.hasMicButton=true;
        result.micRect=rectText(mic[0]);
    }
    return result;
}
function qtReadCheckMessagesToolState(channelId,root,allowGlobal){
    channelId=String(channelId||"").trim();
    if(!channelId)return null;
    var toolName="qtwx-mcp-"+channelId;
    var exactRe=new RegExp("(Running|Ran|Cancelled|Canceled|Failed|Errored)\\s*Check\\s*Messages\\s*in\\s*"+qtEscapeRegExp(toolName),"i");
    var compactTool=toolName.toLowerCase();
    var selectors=[
        '[data-message-kind="tool"]',
        '[data-tool-status]',
        '[data-mcp-tool-status]',
        '.composer-tool-call-content',
        '.composer-tool-call-container',
        '.composer-mcp-tool-call-block'
    ].join(",");
    function collect(scope){
        try{return Array.prototype.slice.call((scope||document).querySelectorAll(selectors)).filter(qtVisibleAny)}catch(e){return []}
    }
    var blocks=collect(root);
    if(allowGlobal)blocks=blocks.concat(collect(document));
    var best=null;
    for(var i=0;i<blocks.length;i++){
        var block=blocks[i];
        var text=qtToolBlockText(block);
        var compact=text.replace(/\s+/g,"").toLowerCase();
        if(compact.indexOf("checkmessages")<0||compact.indexOf(compactTool)<0)continue;
        var status="";
        var m=text.match(exactRe);
        if(m&&m[1])status=m[1];
        var attrStatus="";
        try{attrStatus=String(block.getAttribute("data-tool-status")||block.getAttribute("data-mcp-tool-status")||"")}catch(e){}
        if(!status&&attrStatus==="loading")status="Running";
        if(!status&&/cancel/i.test(attrStatus))status="Cancelled";
        if(!status&&/fail|error/i.test(attrStatus))status="Failed";
        if(!status)continue;
        var running=/^Running$/i.test(status);
        var score=running?100:50;
        try{
            var r=block.getBoundingClientRect();
            score+=Math.max(0,1000-Math.round(r.top))/100;
            if(r.width>100&&r.height>10&&r.height<180)score+=10;
        }catch(e){}
        var item={
            ok:true,
            channelId:channelId,
            toolName:toolName,
            status:status,
            running:running,
            evidence:(running?"running_check_messages:":"check_messages_status:")+toolName+":"+status,
            source:root&&root.contains&&root.contains(block)?"root":"global",
            text:String(text||"").slice(0,800),
            tag:block&&block.tagName||"",
            className:block?String(block.className||"").slice(0,240):"",
            messageKind:block&&block.getAttribute&&block.getAttribute("data-message-kind")||"",
            toolStatus:attrStatus,
            score:score
        };
        if(!best||item.score>best.score)best=item;
    }
    return best;
}
function qtReadRunningCheckMessagesEvidence(composerId,channelId,allowLoose){
    composerId=String(composerId||"").trim();
    channelId=String(channelId||"").trim();
    if(!composerId||!channelId)return null;
    var root=qtFindComposerRootById(composerId);
    var state=qtReadCheckMessagesToolState(channelId,root,allowLoose);
    if(state&&state.running)return state;
    if(!root)return null;
    var toolName="qtwx-mcp-"+channelId;
    var exactRe=new RegExp("Running\\s*Check\\s*Messages\\s*in\\s*"+qtEscapeRegExp(toolName),"i");
    var compactNeedle=("RunningCheckMessagesin"+toolName).toLowerCase();
    var selectors=[
        '[data-message-kind="tool"][data-tool-status="loading"]',
        '[data-mcp-tool-status="loading"]',
        '.composer-mcp-tool-call-block.active',
        '.composer-tool-call-container.active'
    ].join(",");
    var blocks=[];
    try{blocks=Array.prototype.slice.call(root.querySelectorAll(selectors)).filter(qtVisibleAny)}catch(e){blocks=[]}
    for(var i=0;i<blocks.length;i++){
        var block=blocks[i];
        var text=qtToolBlockText(block);
        var compact=text.replace(/\s+/g,"").toLowerCase();
        if(exactRe.test(text)||compact.indexOf(compactNeedle)>=0){
            return qtRunningCheckEvidence(block,channelId,toolName,text,"strict");
        }
    }
    if(allowLoose){
        var looseSelectors=[
            '[data-message-kind]',
            '[data-tool-status]',
            '[data-mcp-tool-status]',
            '[class*="tool"]',
            '[class*="mcp"]',
            '[role="status"]',
            '[aria-live]'
        ].join(",");
        var loose=[];
        try{loose=Array.prototype.slice.call(root.querySelectorAll(looseSelectors)).filter(qtVisibleAny)}catch(e){loose=[]}
        for(var j=loose.length-1;j>=0;j--){
            var looseBlock=loose[j];
            var looseText=qtToolBlockText(looseBlock);
            var looseCompact=looseText.replace(/\s+/g,"").toLowerCase();
            if(exactRe.test(looseText)||looseCompact.indexOf(compactNeedle)>=0){
                return qtRunningCheckEvidence(looseBlock,channelId,toolName,looseText,"loose");
            }
        }
        var rootText=qtToolBlockText(root);
        var rootCompact=rootText.replace(/\s+/g,"").toLowerCase();
        if(exactRe.test(rootText)||rootCompact.indexOf(compactNeedle)>=0){
            return qtRunningCheckEvidence(root,channelId,toolName,rootText,"root");
        }
    }
    return null;
}
function qtElementKey(el){
    if(!el)return "";
    var attrs=["data-message-id","data-id","data-testid","id"];
    for(var i=0;i<attrs.length;i++){
        var v=el.getAttribute&&el.getAttribute(attrs[i]);
        if(v)return attrs[i]+"="+v;
    }
    return "";
}
function qtHumanMessages(root){
    if(!root)return [];
    return Array.prototype.slice.call(root.querySelectorAll(".composer-human-message")).filter(qtVisibleAny);
}
function qtCaptureHumanMessageAnchor(root,text,beforeCount){
    var expectedHash=qtTextHash(text);
    var list=qtHumanMessages(root);
    var candidates=list.filter(function(el,idx){
        var body=qtNormalizeText(el.innerText||el.textContent||"");
        return body&&qtTextHash(body)===expectedHash&&idx>=beforeCount;
    });
    if(candidates.length!==1){
        candidates=list.filter(function(el,idx){
            var body=qtNormalizeText(el.innerText||el.textContent||"");
            var expected=qtNormalizeText(text);
            return body&&expected&&(body.indexOf(expected)>=0||expected.indexOf(body)>=0)&&idx>=beforeCount;
        });
    }
    if(candidates.length!==1)return null;
    var el=candidates[0];
    var index=list.indexOf(el);
    return {
        key:qtElementKey(el),
        index:index,
        textHash:expectedHash,
        textPreview:qtNormalizeText(text).slice(0,160)
    };
}
function qtWaitHumanMessageAnchor(root,text,beforeCount,timeoutMs){
    return new Promise(function(resolve){
        var started=Date.now();
        function check(){
            var anchor=qtCaptureHumanMessageAnchor(root,text,beforeCount);
            if(anchor||Date.now()-started>=timeoutMs){
                resolve(anchor);
                return;
            }
            setTimeout(check,150);
        }
        check();
    });
}
function qtFindHumanMessageByAnchor(root,anchor){
    if(!root||!anchor)return null;
    var list=qtHumanMessages(root);
    if(anchor.key){
        for(var i=0;i<list.length;i++){
            if(qtElementKey(list[i])===anchor.key)return list[i];
        }
        return null;
    }
    var matches=list.filter(function(el,idx){
        if(typeof anchor.index==="number"&&idx!==anchor.index)return false;
        var body=qtNormalizeText(el.innerText||el.textContent||"");
        return body&&qtTextHash(body)===anchor.textHash;
    });
    return matches.length===1?matches[0]:null;
}
function qtClickButton(btn){
    try{btn.focus()}catch(e){}
    var r;
    try{r=btn.getBoundingClientRect()}catch(e){r=null}
    var x=r?Math.floor((r.left+r.right)/2):0;
    var y=r?Math.floor((r.top+r.bottom)/2):0;
    // ⚠️ 关键：绝对不要再调 btn.click()。对 <a class="action-label">/<button> 这种元素，
    //     dispatch click + btn.click() 会触发两次 onClick —— 之前 New Agent
    //     一次点击开两个窗口就是这个双击 bug
    ["pointerdown","mousedown","pointerup","mouseup","click"].forEach(function(type){
        try{
            var Ctor=type.indexOf("pointer")===0?PointerEvent:MouseEvent;
            btn.dispatchEvent(new Ctor(type,{
                bubbles:true,cancelable:true,composed:true,view:window,
                button:0,buttons:type==="pointerdown"||type==="mousedown"?1:0,
                clientX:x,clientY:y,screenX:x,screenY:y,
                detail:type==="click"?1:0
            }));
        }catch(e){}
    });
}
function qtButtonLabel(el){
    return String(el&&(el.innerText||el.textContent)||"").replace(/\s+/g," ").trim();
}
function qtSendPromptToCursor(text, task){
    task=task||{};
    var channelId=String(task.channelId||"").trim();
    var targetMode=String(task.targetMode||"active");
    var targetComposerId=String(task.composerId||"").trim();
    if(targetMode==="bound"&&!targetComposerId&&channelId){
        targetComposerId=qtReadBoundComposerId(channelId);
    }
    var input=null;
    var composerId="";
    if(targetMode==="bound"){
        var root=qtFindComposerRootById(targetComposerId);
        if(!root){
            _qtLog("prompt: 未找到 CH-"+channelId+" 历史绑定 composer "+targetComposerId);
            return Promise.resolve({ok:false,error:"bound composer not found",retryable:false,skip:true});
        }
        input=qtFindInputInComposerRoot(root);
        composerId=qtComposerIdFromRoot(root)||targetComposerId;
    }else{
        input=qtFindComposerInput();
        composerId=qtComposerIdFromInput(input);
    }
    if(!input){_qtLog("prompt: 找不到输入框");return Promise.resolve({ok:false,error:"composer input not found"})}
    _qtLog("prompt: 选中输入框 tag="+input.tagName+" cls="+(input.className||"").slice(0,40));
    var beforeRoot=targetMode==="bound"?qtFindComposerRootById(targetComposerId):qtComposerRootFromInput(input);
    var beforeCount=beforeRoot?qtHumanMessages(beforeRoot).length:0;
    qtSetInputValue(input,text);
    _qtLog("prompt: 单次注入完成");
    return new Promise(function(resolve){
        var started=Date.now();
        var clicked=false;
        function attempt(){
            // 使用 NearInput 版：保证拿到的是该 input 同 Composer 的按钮
            var button=qtFindSendButtonNearInput(input);
            if(button&&!button.disabled&&button.getAttribute("aria-disabled")!=="true"){
                if(!clicked){_qtLog("prompt: 找到按钮 tag="+button.tagName+" cls="+(button.className||"").slice(0,40)+"，点击")}
                clicked=true;
                qtClickButton(button);
                setTimeout(function(){
                    var after=qtReadInputText(input);
                    var needNative=String(after||"").trim().length>0;
                    var finish=function(anchor){
                        if(channelId&&composerId)qtBindChannelComposer(channelId,composerId,"auto_prompt_submit");
                        _qtLog("prompt: 点击后输入框长度="+(after||"").length+(needNative?" ⚠️仍有内容，需 native Enter":" ✅已发送"));
                        resolve({ok:true,method:"button",needsNativeEnter:needNative,composerId:composerId,humanAnchor:anchor||null});
                    };
                    if(task.captureHumanAnchor&&beforeRoot){
                        qtWaitHumanMessageAnchor(beforeRoot,text,beforeCount,2500).then(finish);
                    }else{
                        finish(null);
                    }
                },650);
                return;
            }
            if(Date.now()-started<2600){
                setTimeout(attempt,120);
                return;
            }
            _qtLog("prompt: 2.6s 内按钮未就绪，走 Enter 兜底");
            qtPressEnter(input);
            setTimeout(function(){
                var after=qtReadInputText(input);
                var needNative=String(after||"").trim().length>0;
                var finish=function(anchor){
                    if(channelId&&composerId)qtBindChannelComposer(channelId,composerId,"auto_prompt_submit");
                    _qtLog("prompt: Enter 后输入框长度="+(after||"").length+(needNative?" ⚠️仍有内容，需 native Enter":" ✅已发送"));
                    resolve({ok:true,method:"enter",needsNativeEnter:needNative,composerId:composerId,humanAnchor:anchor||null});
                };
                if(task.captureHumanAnchor&&beforeRoot){
                    qtWaitHumanMessageAnchor(beforeRoot,text,beforeCount,2500).then(finish);
                }else{
                    finish(null);
                }
            },650);
        }
        setTimeout(attempt,160);
    });
}
var qtPromptBusy=false;
var qtPromptDone={};
function qtMakeClientId(){
    return "qt-"+Date.now().toString(36)+"-"+Math.random().toString(36).slice(2,8);
}
function qtInitClientId(){
    try{
        if(window.__qingtian_runtime_client_id)return String(window.__qingtian_runtime_client_id);
        var id=qtMakeClientId();
        window.__qingtian_runtime_client_id=id;
        return id;
    }catch(e){
        return qtMakeClientId();
    }
}
var qtClientId=qtInitClientId();
_qtRuntimeHeartbeat();
var qtPromptFailures=0;
var qtPromptBackoffUntil=0;
function qtPromptOk(){qtPromptFailures=0;qtPromptBackoffUntil=0}
function qtPromptFailed(){
    qtPromptFailures=Math.min(qtPromptFailures+1,6);
    qtPromptBackoffUntil=Date.now()+Math.min(30000,1000*Math.pow(2,qtPromptFailures-1));
}
function qtWindowFocused(){
    try{return !!document.hasFocus()}catch(e){return false}
}
function pollStartPrompt(){
    if(_qtStopped)return;
    if(qtPromptBusy)return;
    if(Date.now()<qtPromptBackoffUntil)return;
    qtPromptBusy=true;
    var focused=qtWindowFocused();
    fetch("http://127.0.0.1:"+PORT+"/api/pending-start-prompt?clientId="+encodeURIComponent(qtClientId)+"&focused="+(focused?"1":"0"))
        .then(function(r){return r.json()})
        .then(function(d){
            qtPromptOk();
            if(!d||!d.id||!d.prompt){qtPromptBusy=false;return}
            if(qtPromptDone[d.id]){qtPromptBusy=false;return}
            if(d.workspaceScopeId)qtSetBatchWorkspaceScopeId(d.workspaceScopeId,"start_prompt");
            _qtLog("prompt: 拉到任务 id="+d.id+" len="+(d.prompt||"").length+" focused="+focused+" target="+(d.targetMode||"active")+" ch="+(d.channelId||""));
            qtSendPromptToCursor(String(d.prompt||""),d).then(function(result){
                if(result&&result.ok)qtPromptDone[d.id]=1;
                _qtLog("prompt: 上报结果 ok="+!!(result&&result.ok)+" method="+(result&&result.method||"-")+" needNative="+!!(result&&result.needsNativeEnter));
                return fetch("http://127.0.0.1:"+PORT+"/api/start-prompt-done",{
                    method:"POST",
                    headers:{"Content-Type":"application/json"},
                    body:JSON.stringify({id:d.id,clientId:qtClientId,focused:focused,ok:!!(result&&result.ok),method:result&&result.method,error:result&&result.error,needsNativeEnter:!!(result&&result.needsNativeEnter),composerId:result&&result.composerId,skip:!!(result&&result.skip)})
                });
            }).catch(function(e){
                _qtLog("prompt: 处理异常 "+(e&&e.message||e));
                return fetch("http://127.0.0.1:"+PORT+"/api/start-prompt-done",{
                    method:"POST",
                    headers:{"Content-Type":"application/json"},
                    body:JSON.stringify({id:d.id,clientId:qtClientId,focused:focused,ok:false,error:String(e&&e.message||e)})
                });
            }).finally(function(){qtPromptBusy=false});
        })
        .catch(function(){qtPromptBusy=false;qtPromptFailed()});
}
_qtSetInterval(pollStartPrompt,700);

// ─── Composer Bridge API ────────────────────────────
${SEAMLESS_COMPOSER_BRIDGE_MARKER}
;(function(){
var _bLog=function(m){try{console.log("[QT-ComposerBridge] "+m)}catch(e){}};
function _svc(){return window.__qtComposerService||null}
function _ds(){var s=_svc();return s&&(s.composerDataService||s._composerDataService)||null}
function _cs(){var s=_svc();return s&&(s.composerChatService||s._composerChatService)||null}
function _safeComposerArray(ds){
    try{
        var arr=ds&&ds.allComposersData&&ds.allComposersData.allComposers;
        if(Array.isArray(arr))return arr;
    }catch(e){}
    try{
        var arr2=ds&&ds._allComposersData&&ds._allComposersData.allComposers;
        if(Array.isArray(arr2))return arr2;
    }catch(e){}
    return [];
}
function _errorDetailsText(details){
    if(!details)return "";
    try{
        var seen=[];
        return JSON.stringify(details,function(key,value){
            if(value&&typeof value==="object"){
                if(value instanceof Error)return {name:value.name||"",message:value.message||"",stack:value.stack||""};
                if(seen.indexOf(value)>=0)return "[Circular]";
                seen.push(value);
            }
            if(typeof value==="function")return "[Function "+(value.name||"anonymous")+"]";
            return value;
        });
    }catch(e){
        try{return String(details&&details.message||details&&details.name||details)}catch(_){return "[unserializable_errorDetails]"}
    }
}
function _bridgeActiveComposerId(){
    try{
        var root=document.activeElement&&document.activeElement.closest?document.activeElement.closest("[data-composer-id]"):null;
        var id=root?qtComposerIdFromRoot(root):"";
        if(id)return id;
    }catch(e){}
    try{
        var ds=_ds();
        var selected=ds&&ds.selectedComposerIds;
        if(selected&&selected.length)return String(selected[selected.length-1]||"").trim();
    }catch(e){}
    return "";
}
function _bridgeComposerIdByGenerationUUID(uuid){
    uuid=String(uuid||"").trim();
    if(!uuid)return "";
    try{
        var ds=_ds();
        var composers=window.__qtComposerBridge&&window.__qtComposerBridge.listComposers?window.__qtComposerBridge.listComposers():[];
        for(var i=0;i<composers.length;i++){
            var cid=String(composers[i]&&composers[i].composerId||"").trim();
            if(!cid)continue;
            var data=null;
            try{if(ds&&ds.getComposerDataIfLoaded)data=ds.getComposerDataIfLoaded(cid)}catch(e){}
            if(data&&String(data.chatGenerationUUID||data.latestChatGenerationUUID||"").trim()===uuid)return cid;
        }
    }catch(e){}
    return "";
}
function _bridgeReleaseComposerWaiting(composerId,reason){
    composerId=String(composerId||"").trim();
    if(!composerId){
        qtReleaseHookLog("bridge_release_missing_composer reason="+(reason||"cursor_stop"));
        return false;
    }
    var resolved=qtResolveChannelIdByComposerIdDetails(composerId);
    qtReleaseHookLog("bridge_release composer="+composerId+" channel="+(resolved.channelId||"")+" source="+(resolved.source||"")+" reason="+(reason||"cursor_stop"));
    return qtReleaseChannelWaiting(resolved.channelId||"",composerId,reason||"cursor_stop",window.__qtBatchRetryPort||26399);
}
function _qtWrapStopMethod(owner,name,resolveComposerId,reason){
    try{
        var key=String(reason||name||"")+"."+String(name||"");
        if(!owner){
            qtStopHookProbeLog(key,"missing_owner");
            return;
        }
        if(typeof owner[name]!=="function"){
            qtStopHookProbeLog(key,"missing_method:"+typeof owner[name]);
            return;
        }
        if(owner[name].__qtReleaseWrapped){
            qtStopHookProbeLog(key,"wrapped");
            return;
        }
        qtStopHookProbeLog(key,"wrapping");
        var original=owner[name];
        var wrapped=function(){
            try{
                var cid=resolveComposerId?resolveComposerId(arguments):"";
                var activeCid="";
                if(!cid){
                    activeCid=_bridgeActiveComposerId();
                    cid=activeCid;
                }
                var matchedInst=null;
                try{matchedInst=cid?qtFindBatchRetryInstanceForRelease("",cid):null}catch(_){}
                var runningFallback=null;
                try{runningFallback=qtFindRunningCheckMessagesChannel(cid||"")}catch(_){}
                qtReleaseHookLog("fired method="+name+" reason="+(reason||name)+" arg0="+String(arguments&&arguments[0]||"").slice(0,120)+" composer="+(cid||"")+" active="+(activeCid||""));
                qtReleaseHookLog("fired_context method="+name+" reason="+(reason||name)+" composer="+(cid||"")+" hasInstance="+(matchedInst?1:0)+" instTask="+(matchedInst&&matchedInst.taskId||"")+" instChannel="+(matchedInst&&matchedInst.channelId||"")+" instRunning="+(matchedInst&&matchedInst.running?1:0)+" instAttempts="+(matchedInst&&matchedInst.attempts||0)+" runningCheck="+(runningFallback&&runningFallback.running?1:0)+" runningChannel="+(runningFallback&&runningFallback.channelId||"")+" runningSource="+(runningFallback&&runningFallback.source||""));
                if(cid){
                    _bridgeReleaseComposerWaiting(cid,reason||name);
                }else{
                    var running=qtFindRunningCheckMessagesChannel("");
                    qtReleaseHookLog("no_composer_fallback method="+name+" channel="+(running&&running.channelId||"")+" status="+(running&&running.status||"")+" source="+(running&&running.source||""));
                    if(running&&running.channelId)qtReleaseChannelWaiting(running.channelId,"",reason||name,window.__qtBatchRetryPort||26399);
                }
            }catch(e){qtReleaseHookLog("failed method="+name+" error="+String(e&&e.message||e))}
            return original.apply(this,arguments);
        };
        try{wrapped.__qtReleaseWrapped=true;wrapped.__qtOriginal=original}catch(e){}
        owner[name]=wrapped;
        qtReleaseHookLog("installed method="+name+" key="+key);
    }catch(e){qtReleaseHookLog("install_failed method="+name+" error="+String(e&&e.message||e))}
}
function _installComposerStopReleaseHooks(){
    var svc=_svc(),cs=_cs();
    _qtWrapStopMethod(svc,"cancelCurrentStep",function(args){return String(args&&args[0]||"").trim()},"cursor_cancel_current_step");
    _qtWrapStopMethod(svc,"cancelAll",function(args){return String(args&&args[0]||"").trim()||_bridgeActiveComposerId()},"cursor_composer_cancel_all");
    _qtWrapStopMethod(cs,"abortGenerationUUID",function(args){return _bridgeComposerIdByGenerationUUID(args&&args[0])},"cursor_abort_generation_uuid");
    _qtWrapStopMethod(cs,"cancelAll",function(args){return String(args&&args[0]||"").trim()||_bridgeActiveComposerId()},"cursor_chat_cancel_all");
}
try{_qtSetInterval(_installComposerStopReleaseHooks,1000);_qtSetTimeout(_installComposerStopReleaseHooks,1200)}catch(e){}

window.__qtComposerBridge={
    get ready(){return !!_svc()},

    // 创建新 agent（静默）
    createAgent:function(prompt,name,opts){
        opts=opts||{};
        var svc=_svc();
        if(!svc||!svc.createComposer){
            _bLog("createAgent: service not ready");
            return Promise.resolve(null);
        }
        var partialState={unifiedMode:"agent"};
        if(prompt)partialState.text=prompt;
        if(prompt)partialState.richText=prompt;
        if(name)partialState.name=name;
        var createOpts={
            skipShowAndFocus:opts.skipShowAndFocus!==false,
            skipFocus:opts.skipFocus!==false,
            skipSelect:opts.skipSelect!==false,
            autoSubmit:!!opts.autoSubmit,
            partialState:partialState
        };
        _bLog("createAgent: creating"+(opts.autoSubmit?" +autoSubmit":""));
        return svc.createComposer(createOpts).then(function(result){
            if(!result||!result.composerId){
                var resultText=_errorDetailsText(result)||"createComposer returned null";
                _bLog("createAgent: createComposer returned "+resultText);
                return {error:resultText};
            }
            var cid=result.composerId;
            _bLog("createAgent: created "+cid);
            // rename if name provided
            if(name){
                var ds=_ds();
                if(ds&&ds.updateComposerDataAsync){
                    ds.updateComposerDataAsync(cid,function(setData){
                        setData("name",name);
                    }).catch(function(e){_bLog("createAgent: rename failed "+e)});
                }
            }
            return {composerId:cid};
        }).catch(function(e){
            var errorText=_errorDetailsText(e)||String(e);
            _bLog("createAgent error: "+errorText);
            return {error:errorText};
        });
    },

    // 向指定 composer 提交文本
    submitByComposerId:function(composerId,text,opts){
        opts=opts||{};
        var cs=_cs();
        if(!cs||!cs.submitChatMaybeAbortCurrent){
            _bLog("submit: chatService not ready");
            return Promise.resolve({ok:false,error:"chatService not ready"});
        }
        _bLog("submit: cid="+composerId+" len="+(text||"").length);
        var submitOpts={};
        var passKeys=["forceBubbleId","isResume","bubbleId","ignoreQueuing"];
        for(var i=0;i<passKeys.length;i++){
            var key=passKeys[i];
            if(opts[key]!==undefined)submitOpts[key]=opts[key];
        }
        return cs.submitChatMaybeAbortCurrent(composerId,text,submitOpts).then(function(){
            _bLog("submit: ok cid="+composerId);
            return {ok:true};
        }).catch(function(e){
            var errorText=_errorDetailsText(e)||String(e);
            _bLog("submit error: "+errorText);
            return {ok:false,error:errorText};
        });
    },

    // 重命名 composer
    renameComposer:function(composerId,name){
        var ds=_ds();
        if(!ds||!ds.updateComposerDataAsync){
            _bLog("rename: dataService not ready");
            return Promise.resolve({ok:false,error:"dataService not ready"});
        }
        return ds.updateComposerDataAsync(composerId,function(setData){
            setData("name",name);
        }).then(function(){
            _bLog("rename: ok cid="+composerId+" name="+name);
            return {ok:true};
        }).catch(function(e){
            _bLog("rename error: "+e);
            return {ok:false,error:String(e)};
        });
    },

    // 列出当前所有 composer
    listComposers:function(){
        var ds=_ds();
        var ids=[];
        var seen={};
        function addId(id){
            id=String(id||"").trim();
            if(!id||seen[id])return;
            seen[id]=true;
            ids.push(id);
        }
        try{
            var all=_safeComposerArray(ds);
            for(var a=0;a<all.length;a++){
                addId(all[a]&&all[a].composerId);
            }
        }catch(e){_bLog("listComposers allComposers ids error: "+e)}
        try{(ds&&ds.selectedComposerIds||[]).forEach(addId)}catch(e){_bLog("listComposers selected ids error: "+e)}
        try{
            var roots=document.querySelectorAll("[data-composer-id]");
            for(var r=0;r<roots.length;r++)addId(roots[r].getAttribute("data-composer-id"));
        }catch(e){_bLog("listComposers dom ids error: "+e)}
        try{
            var stores=[
                ds&&ds.composerData,
                ds&&ds.composerDataMap,
                ds&&ds.composers,
                ds&&ds.composerMap,
                ds&&ds._composerData,
                ds&&ds._composerDataMap,
                ds&&ds._composers
            ];
            for(var s=0;s<stores.length;s++){
                var store=stores[s];
                if(!store)continue;
                if(store instanceof Map){
                    store.forEach(function(value,key){
                        addId(key);
                        if(value)addId(value.composerId||value.id);
                    });
                }else if(typeof store==="object"){
                    Object.keys(store).forEach(function(key){
                        addId(key);
                        var value=store[key];
                        if(value)addId(value.composerId||value.id);
                    });
                }
            }
        }catch(e){_bLog("listComposers store ids error: "+e)}
        var result=[];
        for(var i=0;i<ids.length;i++){
            var cid=ids[i];
            try{
                var data=null;
                if(ds&&ds.getComposerDataIfLoaded)data=ds.getComposerDataIfLoaded(cid);
                if(!data){
                    var allData=_safeComposerArray(ds);
                    for(var j=0;j<allData.length;j++){
                        if(allData[j]&&allData[j].composerId===cid){data=allData[j];break}
                    }
                }
                result.push({
                    composerId:cid,
                    name:(data&&data.name)||"",
                    status:(data&&data.status)||"unknown",
                    unifiedMode:(data&&data.unifiedMode)||"",
                    text:(data&&data.text)||"",
                    isGenerating:!!(data&&data.isGenerating),
                    createdAt:(data&&data.createdAt)||0
                });
            }catch(e){
                result.push({composerId:cid,name:"",status:"error"});
            }
        }
        _bLog("listComposers count="+result.length);
        return result;
    },

    // 获取最近创建的 composer
    getRecentlyCreatedComposers:function(sinceMs){
        sinceMs=sinceMs||60000;
        var all=this.listComposers();
        var cutoff=Date.now()-sinceMs;
        return all.filter(function(c){return c.createdAt&&c.createdAt>cutoff});
    },

    // 获取指定 composer 的数据
    getComposerData:function(composerId){
        var ds=_ds();
        if(!ds)return null;
        try{
            if(ds.getComposerDataIfLoaded)return ds.getComposerDataIfLoaded(composerId);
        }catch(e){_bLog("getComposerData error: "+e)}
        return null;
    },

    // 关闭 composer
    getStatus:function(composerId,channelId){
        var ds=_ds();
        var data=null,lastHuman=null,lastAi=null,aiCount=0,runningCheckMessages=null;
        try{if(ds&&ds.getComposerDataIfLoaded)data=ds.getComposerDataIfLoaded(composerId)}catch(e){data=null}
        try{if(channelId)runningCheckMessages=qtReadRunningCheckMessagesEvidence(composerId,channelId,/generating|applying|reading|running|streaming|thinking|tool|pending/.test(String(data&&data.status||"").toLowerCase()))}catch(e){runningCheckMessages=null}
        try{
            var root=qtFindComposerRootById(composerId);
            if(root){
                var humans=Array.prototype.slice.call(root.querySelectorAll(".composer-human-message")).filter(qtVisibleAny);
                var ais=Array.prototype.slice.call(root.querySelectorAll(".composer-ai-message,.composer-assistant-message,[data-message-role='assistant']")).filter(qtVisibleAny);
                lastHuman=humans.length?humans[humans.length-1]:null;
                lastAi=ais.length?ais[ais.length-1]:null;
                aiCount=ais.length;
            }
        }catch(e){}
        try{
            var handle=null;
            if(ds&&ds.getHandleIfLoaded_MIGRATED)handle=ds.getHandleIfLoaded_MIGRATED(composerId);
            if(!handle&&ds&&ds.getHandleIfLoaded)handle=ds.getHandleIfLoaded(composerId);
            if(handle&&ds&&ds.getLastHumanBubble&&!lastHuman)lastHuman=ds.getLastHumanBubble(handle);
            if(handle&&ds&&ds.getLastAiBubbles){
                var aiBubbles=ds.getLastAiBubbles(handle)||[];
                if(Array.isArray(aiBubbles)){
                    aiCount=Math.max(aiCount,aiBubbles.length);
                    if(aiBubbles.length)lastAi=aiBubbles[aiBubbles.length-1];
                }
            }
        }catch(e){}
        return {
            composerId:composerId,
            found:!!(data||lastHuman||lastAi),
            status:(data&&data.status)||"",
            text:(data&&data.text)||"",
            lastUpdatedAt:(data&&data.lastUpdatedAt)||0,
            chatGenerationUUID:(data&&(data.chatGenerationUUID||data.latestChatGenerationUUID))||"",
            lastHumanText:(lastHuman&&(lastHuman.text||lastHuman.innerText||lastHuman.textContent))||"",
            lastHumanBubbleId:(lastHuman&&(lastHuman.bubbleId||lastHuman.id||lastHuman.getAttribute&&lastHuman.getAttribute("data-id")))||"",
            lastAiText:(lastAi&&(lastAi.text||lastAi.innerText||lastAi.textContent))||"",
            lastAiBubbleId:(lastAi&&(lastAi.bubbleId||lastAi.id||lastAi.getAttribute&&lastAi.getAttribute("data-id")))||"",
            aiBubbleCount:aiCount,
            runningCheckMessages:runningCheckMessages,
            hasError:!!(lastAi&&lastAi.errorDetails),
            errorText:_errorDetailsText(lastAi&&lastAi.errorDetails)
        };
    },

    stopByComposerId:function(composerId){
        var svc=_svc(),cs=_cs(),ds=_ds(),methods=[];
        try{_bridgeReleaseComposerWaiting(composerId,"bridge_stop_by_composer")}catch(e){}
        return Promise.resolve().then(function(){
            if(svc&&typeof svc.cancelCurrentStep==="function"){methods.push("cancelCurrentStep");return svc.cancelCurrentStep(composerId,{focusBottomInput:false})}
        }).catch(function(e){_bLog("stop cancelCurrentStep failed: "+e)}).then(function(){
            if(svc&&typeof svc.cancelAll==="function"){methods.push("composerCancelAll");return svc.cancelAll(composerId)}
        }).catch(function(e){_bLog("stop cancelAll failed: "+e)}).then(function(){
            var data=null;
            try{if(ds&&ds.getComposerDataIfLoaded)data=ds.getComposerDataIfLoaded(composerId)}catch(e){}
            if(data&&data.chatGenerationUUID&&cs&&typeof cs.abortGenerationUUID==="function"){methods.push("abortGenerationUUID");return cs.abortGenerationUUID(data.chatGenerationUUID)}
        }).catch(function(e){_bLog("stop abortGenerationUUID failed: "+e)}).then(function(){
            if(cs&&typeof cs.cancelAll==="function"){methods.push("chatCancelAll");return cs.cancelAll(composerId)}
        }).catch(function(e){_bLog("stop chatCancelAll failed: "+e)}).then(function(){
            return methods.length?{ok:true,method:methods.join("+")}:{ok:false,error:"composer_stop_method_missing"};
        });
    },

    closeComposer:function(composerId){
        var svc=_svc();
        if(!svc||!svc.closeComposer){
            return Promise.resolve({ok:false,error:"service not ready"});
        }
        return svc.closeComposer(composerId).then(function(){
            return {ok:true};
        }).catch(function(e){
            return {ok:false,error:String(e)};
        });
    },

    // 激活/切换到指定 composer（让其在 DOM 中渲染）
    activateComposer:function(composerId){
        if(!composerId)return Promise.resolve({ok:false,error:"no_id"});
        var svc=_svc();
        var ds=_ds();
        // 多策略尝试，谁先成就用谁
        var tryFns=[
            function(){if(svc&&svc.openComposer)return svc.openComposer(composerId);},
            function(){if(svc&&svc.selectComposer)return svc.selectComposer(composerId);},
            function(){if(svc&&svc.showComposer)return svc.showComposer(composerId);},
            function(){if(svc&&svc.setActiveComposer)return svc.setActiveComposer(composerId);},
            function(){if(ds&&ds.selectComposer)return ds.selectComposer(composerId);},
            function(){if(ds&&ds.setSelectedComposerIds)return ds.setSelectedComposerIds([composerId]);},
            function(){
                // 兜底：通过 createComposer 的 existingId 复用并显示
                if(svc&&svc.createComposer){
                    return svc.createComposer({
                        existingId:composerId,
                        skipShowAndFocus:false,
                        skipFocus:false,
                        skipSelect:false
                    });
                }
            }
        ];
        return new Promise(function(resolve){
            var idx=0;
            var tryNext=function(){
                if(idx>=tryFns.length){
                    _bLog("activateComposer: all strategies failed for "+composerId);
                    resolve({ok:false,error:"no_strategy_worked"});
                    return;
                }
                var fn=tryFns[idx++];
                try{
                    var ret=fn();
                    if(ret&&typeof ret.then==="function"){
                        ret.then(function(){
                            _bLog("activateComposer: strategy "+(idx-1)+" ok for "+composerId);
                            resolve({ok:true,strategy:idx-1});
                        }).catch(function(e){
                            _bLog("activateComposer: strategy "+(idx-1)+" failed: "+e);
                            tryNext();
                        });
                    }else if(ret!==undefined){
                        _bLog("activateComposer: strategy "+(idx-1)+" sync ok for "+composerId);
                        resolve({ok:true,strategy:idx-1});
                    }else{
                        tryNext();
                    }
                }catch(e){
                    _bLog("activateComposer: strategy "+(idx-1)+" threw: "+e);
                    tryNext();
                }
            };
            tryNext();
        });
    }
};
_bLog("Bridge API mounted, ready="+window.__qtComposerBridge.ready);
})();

// ─── Batch Retry Client Script ──────────────────────
;(function(){
"use strict";
var _TAG="[QT-Retry]";
var _rLog=function(m){try{console.log(_TAG+" "+m)}catch(e){}try{_postRetryLog(m)}catch(e){}};
var _rWarn=function(m){try{console.warn(_TAG+" "+m)}catch(e){try{console.log(_TAG+" "+m)}catch(_){}}try{_postRetryLog("WARN "+m)}catch(e){}};
var _SILENT_CREATE_TIMEOUT_MS=15000;
var _SILENT_VISIBLE_SUBMIT_TIMEOUT_MS=15000;
var _SILENT_INITIAL_BRIDGE_SUBMIT_TIMEOUT_MS=30000;
var _SILENT_BRIDGE_RESUME_TIMEOUT_MS=2500;
var _SILENT_NOT_STARTED_MAX=6;
var _SILENT_NOT_STARTED_MIN_MS=12000;

function _silentLaunchDetailsText(details){
    if(!details)return "";
    try{
        var seen=[];
        return JSON.stringify(details,function(key,value){
            if(value&&typeof value==="object"){
                if(value instanceof Error)return {name:value.name||"",message:value.message||"",stack:value.stack||""};
                if(seen.indexOf(value)>=0)return "[Circular]";
                seen.push(value);
            }
            if(typeof value==="function")return "[Function "+(value.name||"anonymous")+"]";
            return value;
        });
    }catch(e){
        try{return String(details&&details.message||details&&details.name||details)}catch(_){return "[unserializable_details]"}
    }
}

function _silentLaunchErrorText(err){
    if(!err)return "unknown_error";
    try{
        if(err&&typeof err==="object"){
            var msg=err.message||err.name||_silentLaunchDetailsText(err);
            return String(msg||err);
        }
    }catch(e){}
    return String(err);
}

function _runSilentLaunchStage(phase,timeoutMs,action){
    var started=Date.now();
    _rLog("silent_launch "+phase+"_begin timeout="+timeoutMs);
    return new Promise(function(resolve){
        var done=false;
        var timer=setTimeout(function(){
            if(done)return;
            done=true;
            var elapsed=Date.now()-started;
            _rWarn("silent_launch "+phase+"_timeout elapsed="+elapsed);
            resolve({ok:false,error:phase+"_timeout",phase:phase,elapsedMs:elapsed,timeout:true});
        },timeoutMs);
        Promise.resolve().then(action).then(function(value){
            if(done)return;
            done=true;
            clearTimeout(timer);
            var elapsed=Date.now()-started;
            _rLog("silent_launch "+phase+"_done elapsed="+elapsed+" result="+_silentLaunchDetailsText(value));
            resolve({ok:true,value:value,phase:phase,elapsedMs:elapsed,timeout:false});
        }).catch(function(err){
            if(done)return;
            done=true;
            clearTimeout(timer);
            var elapsed=Date.now()-started;
            var error=_silentLaunchErrorText(err);
            _rWarn("silent_launch "+phase+"_error elapsed="+elapsed+" error="+error);
            resolve({ok:false,error:error,phase:phase,elapsedMs:elapsed,timeout:false});
        });
    });
}

// ─── 配置 ───────────────────────────────────────
var _DEFAULT_OPTS={LIMIT:500,GONE_SETTLE_MS:360,GONE_SETTLE_CAP:6000,APPEAR_SETTLE_MS:480,COMPOSER_SUCCESS_STABLE_MS:2500,COMPOSER_CONFIRM_SUCCESS_MAX_MS:12000,COMPOSER_STALLED_RETRY_MS:45000,COMPOSER_ATTEMPT_RETRY_MS:45000,COMPOSER_STOP_TIMEOUT_MS:30000,RETRY_IDLE_MS:1500};
var _OPTS=(function(){
    var o=Object.assign({},_DEFAULT_OPTS);
    try{
        var raw=localStorage.getItem("__qt_retry_opts");
        if(raw){var p=JSON.parse(raw);for(var k in p){if(typeof p[k]==="number"&&p[k]>0)o[k]=p[k]}}
    }catch(e){}
    return o;
})();

// ─── DOM 选择器 ──────────────────────────────────
var _SEL_COMPOSER_ROOT="[data-composer-id]";
var _SEL_POPUP=".composer-warning-popup";
var _SEL_STICKY=".composer-sticky-human-message";
var _SEL_EDITOR='.aislash-editor-input[contenteditable="true"]';
var _SEL_SEND=".send-with-mode .anysphere-icon-button";

// ─── 状态 ────────────────────────────────────────
var _instances={};   // {composerId: {running,attempts,generation}}
function _postRetryLog(message){
    try{
        if(!window.XMLHttpRequest||!_batchRetryPort)return;
        var xhr=new XMLHttpRequest();
        xhr.open("POST","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/log",true);
        xhr.setRequestHeader("Content-Type","application/json");
        xhr.timeout=1200;
        xhr.send(JSON.stringify({clientId:qtClientId,message:String(message||"")}));
    }catch(e){}
}
function _qtDiagText(value,max){
    try{
        var text=String(value||"").replace(/\\s+/g," ").trim();
        max=max||180;
        return text.length>max?text.slice(0,max):text;
    }catch(e){return ""}
}
function _qtDiagVisible(el){
    try{
        if(!el)return false;
        var rect=el.getBoundingClientRect();
        var style=window.getComputedStyle?window.getComputedStyle(el):null;
        return !!(rect&&rect.width>0&&rect.height>0)&&(!style||style.visibility!=="hidden"&&style.display!=="none"&&Number(style.opacity||1)>0);
    }catch(e){return false}
}
function _qtDiagElement(el){
    if(!el)return null;
    var rect=null;
    try{rect=el.getBoundingClientRect()}catch(e){}
    return {
        tag:String(el.tagName||""),
        text:_qtDiagText(el.textContent||"",120),
        aria:_qtDiagText(el.getAttribute&&el.getAttribute("aria-label")||"",160),
        title:_qtDiagText(el.getAttribute&&el.getAttribute("title")||"",160),
        cls:_qtDiagText(el.className||"",180),
        role:_qtDiagText(el.getAttribute&&el.getAttribute("role")||"",80),
        disabled:!!el.disabled,
        visible:_qtDiagVisible(el),
        rect:rect?{x:Math.round(rect.x),y:Math.round(rect.y),w:Math.round(rect.width),h:Math.round(rect.height)}:null
    };
}
function _collectComposerBridgeDiag(reason,phase,cmd,triggerInfo){
    var diag={
        reason:String(reason||""),
        phase:String(phase||""),
        clientId:String(qtClientId||""),
        batchRetryPort:_batchRetryPort,
        sid:String(cmd&&cmd.sid||""),
        replyId:String(cmd&&cmd.replyId||cmd&&cmd.reqId||""),
        channelId:String(cmd&&cmd.channelId||""),
        visibleSubmit:!!(cmd&&cmd.visibleSubmit),
        reuseExisting:!!(cmd&&cmd.reuseExisting),
        href:"",
        title:"",
        readyState:"",
        focused:false,
        hasBridge:false,
        bridgeReady:false,
        bridgeReadyError:"",
        bridgeKeys:[],
        hasCreateAgent:false,
        hasListComposers:false,
        hasSubmitToComposer:false,
        bridgeComposerCount:null,
        bridgeComposerSample:[],
        listComposersError:"",
        hasComposerService:false,
        hasCreateComposer:false,
        hasComposerDataService:false,
        hasComposerChatService:false,
        serviceKeys:[],
        composerRootCount:0,
        visibleComposerIds:[],
        activeComposerId:"",
        newAgentCandidates:[],
        triggerInfo:triggerInfo||null,
        collectedAt:Date.now()
    };
    try{diag.href=String(location&&location.href||"")}catch(e){}
    try{diag.title=String(document&&document.title||"")}catch(e){}
    try{diag.readyState=String(document&&document.readyState||"")}catch(e){}
    try{diag.focused=!!document.hasFocus()}catch(e){}
    var bridge=null,svc=null,ds=null,cs=null;
    try{bridge=window.__qtComposerBridge||null}catch(e){}
    try{svc=window.__qtComposerService||null}catch(e){}
    try{ds=svc&&(svc.composerDataService||svc._composerDataService)||null}catch(e){}
    try{cs=svc&&(svc.composerChatService||svc._composerChatService)||null}catch(e){}
    diag.hasBridge=!!bridge;
    try{diag.bridgeReady=!!(bridge&&bridge.ready)}catch(e){diag.bridgeReadyError=String(e&&e.message||e)}
    try{diag.bridgeKeys=bridge?Object.keys(bridge).slice(0,40):[]}catch(e){}
    diag.hasCreateAgent=!!(bridge&&typeof bridge.createAgent==="function");
    diag.hasListComposers=!!(bridge&&typeof bridge.listComposers==="function");
    diag.hasSubmitToComposer=!!(bridge&&typeof bridge.submitByComposerId==="function");
    if(bridge&&typeof bridge.listComposers==="function"){
        try{
            var list=bridge.listComposers()||[];
            diag.bridgeComposerCount=list.length;
            diag.bridgeComposerSample=list.slice(0,6).map(function(c){
                return {
                    composerId:String(c&&c.composerId||"").slice(0,12),
                    name:_qtDiagText(c&&c.name||"",80),
                    status:String(c&&c.status||""),
                    unifiedMode:String(c&&c.unifiedMode||"")
                };
            });
        }catch(e){diag.listComposersError=String(e&&e.message||e)}
    }
    diag.hasComposerService=!!svc;
    diag.hasCreateComposer=!!(svc&&typeof svc.createComposer==="function");
    diag.hasComposerDataService=!!ds;
    diag.hasComposerChatService=!!cs;
    try{diag.serviceKeys=svc?Object.keys(svc).slice(0,60):[]}catch(e){}
    try{
        var roots=document.querySelectorAll(_SEL_COMPOSER_ROOT);
        diag.composerRootCount=roots.length;
        for(var i=0;i<roots.length&&diag.visibleComposerIds.length<8;i++){
            if(qtVisibleAny(roots[i])){
                var cid=qtComposerIdFromRoot(roots[i]);
                if(cid)diag.visibleComposerIds.push(cid.slice(0,12));
            }
        }
    }catch(e){}
    try{
        var activeRoot=document.activeElement&&document.activeElement.closest?document.activeElement.closest(_SEL_COMPOSER_ROOT):null;
        var activeId=activeRoot?qtComposerIdFromRoot(activeRoot):"";
        diag.activeComposerId=activeId?activeId.slice(0,12):"";
    }catch(e){}
    try{
        var nodes=document.querySelectorAll('[aria-label*="New Agent" i],[aria-label*="new agent" i],[title*="New Agent" i],[title*="new agent" i]');
        for(var j=0;j<nodes.length&&j<12;j++)diag.newAgentCandidates.push(_qtDiagElement(nodes[j]));
    }catch(e){
        diag.newAgentQueryError=String(e&&e.message||e);
    }
    return diag;
}
function _logComposerBridgeDiag(reason,phase,cmd,triggerInfo){
    var diag=_collectComposerBridgeDiag(reason,phase,cmd,triggerInfo);
    _rWarn("composer_bridge_diag "+_silentLaunchDetailsText(diag));
    return diag;
}
var _genCounter=0;
var _aborted=false;
var _retryPort=PORT;  // 复用注入脚本的 PORT 变量作为 seamless 端口
var _batchRetryDefaultPort=${DEFAULT_BATCH_RETRY_PORT};
var _batchRetryPort=_batchRetryDefaultPort;
var _batchRetryPortScanMax=20;
var _batchRetryDiscovering=false;
var _batchRetryLastDiscoverAt=0;
var _watchedMap={};  // {composerId: {sid,throttleMs,lastCheck}}
var _watchVersion=0;
try{window.__qtBatchRetryPort=_batchRetryPort}catch(e){}

function _releaseRuntimeChannel(channelId,composerId,reason){
    channelId=String(channelId||"").trim();
    composerId=String(composerId||"").trim();
    if(!channelId&&composerId)channelId=qtResolveChannelIdByComposerId(composerId);
    if(!channelId&&!composerId)return false;
    _rLog("release runtime channel ch="+(channelId||"?")+" composer="+(composerId||"")+" reason="+(reason||""));
    return qtReleaseChannelWaiting(channelId,composerId,reason||"client_stop",_batchRetryPort);
}

function _discoverBatchRetryPort(done,force){
    try{
        if(!window.XMLHttpRequest){if(done)done();return}
        var now=Date.now();
        if(_batchRetryDiscovering||(!force&&now-_batchRetryLastDiscoverAt<4000)){if(done)done();return}
        _batchRetryDiscovering=true;
        _batchRetryLastDiscoverAt=now;
        var start=_batchRetryDefaultPort;
        var end=start+_batchRetryPortScanMax-1;
        var best=0;
        function finish(port){
            if(port&&port!==_batchRetryPort){
                _batchRetryPort=port;
                try{window.__qtBatchRetryPort=_batchRetryPort}catch(e){}
                _rLog("Batch retry bridge port switched to "+port);
            }
            _batchRetryDiscovering=false;
            if(done)done();
        }
        function scan(port){
            if(port>end){finish(best);return}
            try{
                var xhr=new XMLHttpRequest();
                xhr.open("GET","http://127.0.0.1:"+port+"/auto-chat/health",true);
                xhr.timeout=700;
                xhr.onload=function(){
                    var data=null;
                    if(xhr.status===200){
                        try{data=JSON.parse(xhr.responseText||"{}")}catch(e){}
                    }
                    if(data&&data.engine==="qingtian"){
                        var healthPort=Number(data.port)||port;
                        if(data.running){finish(healthPort);return}
                        if(!best)best=healthPort;
                    }
                    scan(port+1);
                };
                xhr.onerror=function(){scan(port+1)};
                xhr.ontimeout=xhr.onerror;
                xhr.send();
            }catch(e){scan(port+1)}
        }
        // 多工作区亲和：先问 primary(36530) 我这个窗口被哪个工作区引擎认领，命中且存活就直连；否则回退端口扫描。
        function fallbackScan(){scan(start)}
        try{
            var axhr=new XMLHttpRequest();
            axhr.open("GET","http://127.0.0.1:"+PORT+"/api/batch-bridge-for?clientId="+encodeURIComponent(qtClientId||""),true);
            axhr.timeout=700;
            axhr.onload=function(){
                var ad=null;
                if(axhr.status===200){try{ad=JSON.parse(axhr.responseText||"{}")}catch(e){}}
                var adoptedPort=ad&&Number(ad.port)||0;
                var adoptedScope=ad&&String(ad.workspaceScopeId||ad.batchWorkspaceScopeId||"").trim();
                if(adoptedScope)qtSetBatchWorkspaceScopeId(adoptedScope,"bridge_lookup");
                if(!adoptedPort){fallbackScan();return}
                var vxhr=new XMLHttpRequest();
                vxhr.open("GET","http://127.0.0.1:"+adoptedPort+"/auto-chat/health",true);
                vxhr.timeout=700;
                vxhr.onload=function(){
                    var hd=null;
                    if(vxhr.status===200){try{hd=JSON.parse(vxhr.responseText||"{}")}catch(e){}}
                    if(hd&&hd.engine==="qingtian"){finish(Number(hd.port)||adoptedPort);return}
                    fallbackScan();
                };
                vxhr.onerror=function(){fallbackScan()};
                vxhr.ontimeout=vxhr.onerror;
                vxhr.send();
            };
            axhr.onerror=function(){fallbackScan()};
            axhr.ontimeout=axhr.onerror;
            axhr.send();
        }catch(e){fallbackScan()}
    }catch(e){_batchRetryDiscovering=false;if(done)done()}
}

// ─── DOM 工具 ────────────────────────────────────
function _composerRootById(id){
    return document.querySelector('[data-composer-id="'+id+'"]');
}
function _findPopup(root){
    return root?root.querySelector(_SEL_POPUP):null;
}
function _findSticky(root){
    return root?root.querySelector(_SEL_STICKY):null;
}
function _findRetryBubble(root){
    if(!root)return null;
    var popup=_findPopup(root);
    if(popup){
        var popupBubble=popup.closest('[data-message-role="human"]');
        if(popupBubble)return popupBubble;
        var popupSticky=popup.closest(_SEL_STICKY);
        if(popupSticky)return popupSticky;
    }
    var all=Array.prototype.slice.call(root.querySelectorAll(_SEL_STICKY)).filter(qtVisibleAny);
    if(all.length)return all[all.length-1];
    return _findSticky(root);
}
function _findEditor(root){
    return root?root.querySelector(_SEL_EDITOR):null;
}
function _findSendBtn(root){
    return root?root.querySelector(_SEL_SEND):null;
}
function _findSendBtnInBubble(bubble){
    if(!bubble)return null;
    var btn=bubble.querySelector(_SEL_SEND);
    if(btn&&qtVisibleAny(btn))return btn;
    var candidates=bubble.querySelectorAll('button,[role="button"],[class*="submit" i],[class*="send" i],[class*="anysphere-icon-button" i],[data-mode]');
    for(var i=0;i<candidates.length;i++){
        if(qtVisibleAny(candidates[i]))return candidates[i];
    }
    return null;
}
function _delay(ms){return new Promise(function(r){setTimeout(r,ms)})}
function _isPreviousMessageDialogText(text){
    text=String(text||"");
    return text.indexOf("Submit from a previous message?")>=0&&
        text.indexOf("Submitting from a previous message will revert file changes")>=0&&
        text.indexOf("Don't revert")>=0;
}
function _findPreviousMessageDialogRoot(button){
    var node=button;
    for(var i=0;node&&i<8;i++){
        if(_isPreviousMessageDialogText(node.innerText||node.textContent||""))return node;
        node=node.parentElement;
    }
    return null;
}
function _isPreviousMessageRevertButton(button){
    var label=qtButtonLabel(button);
    if(!/^Revert\b/i.test(label))return false;
    if(/don't\s+revert/i.test(label))return false;
    return !!_findPreviousMessageDialogRoot(button);
}
function _clickPreviousMessageRevert(){
    var candidates=document.querySelectorAll("button,[role='button'],a");
    for(var i=0;i<candidates.length;i++){
        if(_isPreviousMessageRevertButton(candidates[i])){
            _rLog("auto click previous-message Revert");
            qtClickButton(candidates[i]);
            return true;
        }
    }
    return false;
}
function _queuePreviousMessageRevertClicks(){
    _clickPreviousMessageRevert();
    [120,350,700,1200].forEach(function(ms){
        setTimeout(_clickPreviousMessageRevert,ms);
    });
}
try{_qtSetInterval(_clickPreviousMessageRevert,350)}catch(e){}
function _retrySleep(id,gen,ms){
    var step=250;
    var waited=0;
    return new Promise(function(resolve){
        function tick(){
            if(!_alive(id,gen))return resolve(false);
            if(waited>=ms)return resolve(true);
            var next=Math.min(step,ms-waited);
            waited+=next;
            setTimeout(tick,next);
        }
        tick();
    });
}
function _alive(id,gen){var inst=_instances[id];return inst&&inst.running&&inst.generation===gen&&!_aborted}
function _retryInstanceDiag(id,inst){
    try{
        inst=inst||_instances[id]||{};
        return "composer="+(id||inst.composerId||"")+" task="+(inst.taskId||"")+" channel="+(inst.channelId||"")+" running="+(inst.running?1:0)+" gen="+(inst.generation||"")+" attempts="+(inst.attempts||0)+" silent="+(inst.silentMode?1:0)+" aborted="+(_aborted?1:0);
    }catch(e){return "composer="+(id||"")+" diag_error="+String(e&&e.message||e)}
}
function _isComposerObservable(root){
    if(!root||!qtVisibleAny(root))return false;
    try{
        var r=root.getBoundingClientRect();
        return r.width>20&&r.height>20&&r.bottom>0&&r.right>0&&r.top<window.innerHeight&&r.left<window.innerWidth;
    }catch(e){return false}
}
async function _ensureObservableComposer(id,reason){
    var root=_composerRootById(id);
    if(_isComposerObservable(root))return root;
    _rLog(id+" composer not observable during "+reason+", re-activating target");
    var bridge=window.__qtComposerBridge;
    if(bridge&&bridge.activateComposer){
        try{await bridge.activateComposer(id)}catch(e){_rLog(id+" activate for observable failed: "+e)}
    }
    var waited=0;
    while(waited<5000){
        root=_composerRootById(id);
        if(_isComposerObservable(root))return root;
        await _delay(200);
        waited+=200;
    }
    return null;
}
async function _confirmNoPopupOnObservableComposer(id,reason){
    var root=await _ensureObservableComposer(id,reason);
    if(!root)return {ok:false,error:"composer_not_observable"};
    if(_findPopup(root))return {ok:true,hasPopup:true,root:root};
    var appeared=await _waitStableAppear(root,_SEL_POPUP,2500,_OPTS.APPEAR_SETTLE_MS);
    return {ok:true,hasPopup:!!appeared,root:root};
}

// ─── 元素监控 ────────────────────────────────────
function _monitorElement(root,sel,wantAppear,timeoutMs){
    return new Promise(function(resolve){
        var done=false;
        var timer=setTimeout(function(){if(!done){done=true;resolve(false)}},timeoutMs);
        var obs=new MutationObserver(function(){
            var el=root.querySelector(sel);
            var found=!!el;
            if(found===wantAppear&&!done){done=true;clearTimeout(timer);obs.disconnect();resolve(true)}
        });
        obs.observe(root,{childList:true,subtree:true});
        // 立即检查
        var el=root.querySelector(sel);
        if((!!el)===wantAppear&&!done){done=true;clearTimeout(timer);obs.disconnect();resolve(true)}
    });
}

function _waitSettled(root,sel,settleMs,capMs){
    return new Promise(function(resolve){
        var start=Date.now();
        var lastSeen=0;
        function check(){
            if(Date.now()-start>capMs){resolve(false);return}
            var el=root.querySelector(sel);
            if(el){lastSeen=Date.now();setTimeout(check,50);return}
            if(Date.now()-lastSeen>=settleMs||lastSeen===0){resolve(true);return}
            setTimeout(check,50);
        }
        check();
    });
}

function _waitStableAppear(root,sel,timeoutMs,settleMs){
    return new Promise(function(resolve){
        var start=Date.now();
        var lastGone=Date.now();
        function check(){
            if(Date.now()-start>timeoutMs){resolve(false);return}
            var el=root.querySelector(sel);
            if(!el){lastGone=Date.now();setTimeout(check,80);return}
            if(Date.now()-lastGone>=settleMs){resolve(true);return}
            setTimeout(check,80);
        }
        check();
    });
}

// ─── 重发 ────────────────────────────────────────
function _resendFromBubble(root,id,attempt){
    return new Promise(function(resolve){
        // 1. 激活人类气泡
        var sticky=_findSticky(root);
        if(!sticky){_rLog(id+" 找不到人类气泡 attempt="+attempt);resolve(false);return}
        var humanMsg=sticky.querySelector(".composer-human-message");
        if(humanMsg){try{humanMsg.click()}catch(e){}}

        // 2. 等 editor 出现
        var editorAttempts=0;
        function waitEditor(){
            var editor=_findEditor(root);
            if(editor){waitSendBtn(editor);return}
            editorAttempts++;
            if(editorAttempts>15){_rLog(id+" editor 等待超时");resolve(false);return}
            setTimeout(waitEditor,200);
        }

        // 3. 等发送按钮
        function waitSendBtn(editor){
            var btnAttempts=0;
            function tryBtn(){
                var btn=_findSendBtn(root);
                if(btn&&!btn.disabled&&btn.getAttribute("aria-disabled")!=="true"){
                    _rLog(id+" 点击发送 attempt="+attempt);
                    qtClickButton(btn);
                    resolve(true);
                    return;
                }
                btnAttempts++;
                if(btnAttempts>10){
                    // 按钮拿不到，用 Enter 兜底
                    _rLog(id+" 发送按钮超时，Enter 兜底 attempt="+attempt);
                    qtPressEnter(editor);
                    resolve(true);
                    return;
                }
                setTimeout(tryBtn,200);
            }
            tryBtn();
        }
        waitEditor();
    });
}

function _editFirstPromptAndResend(root,inst,attempt){
    return new Promise(function(resolve){
        var anchor=inst&&inst.firstPromptAnchor;
        if(!anchor){resolve({ok:false,error:"missing_first_prompt_anchor"});return}
        var humanMsg=qtFindHumanMessageByAnchor(root,anchor);
        if(!humanMsg){resolve({ok:false,error:"first_prompt_anchor_not_found"});return}
        var body=qtNormalizeText(humanMsg.innerText||humanMsg.textContent||"");
        if(!body||qtTextHash(body)!==anchor.textHash){
            resolve({ok:false,error:"first_prompt_anchor_mismatch"});
            return;
        }
        try{humanMsg.click()}catch(e){}

        var editorAttempts=0;
        function waitEditor(){
            var editor=_findEditor(root);
            if(editor){waitSendBtn(editor);return}
            editorAttempts++;
            if(editorAttempts>15){resolve({ok:false,error:"first_prompt_editor_timeout"});return}
            setTimeout(waitEditor,200);
        }

        function waitSendBtn(editor){
            var btnAttempts=0;
            function tryBtn(){
                var btn=_findSendBtn(root);
                if(btn&&!btn.disabled&&btn.getAttribute("aria-disabled")!=="true"){
                    _rLog(inst.taskId+" edit first prompt resend attempt="+attempt);
                    qtClickButton(btn);
                    resolve({ok:true,method:"edit_first"});
                    return;
                }
                btnAttempts++;
                if(btnAttempts>10){
                    qtPressEnter(editor);
                    resolve({ok:true,method:"edit_first_enter"});
                    return;
                }
                setTimeout(tryBtn,200);
            }
            tryBtn();
        }
        waitEditor();
    });
}

// ─── 触发底部发送 ────────────────────────────────
function _resendFromBubble(root,id,attempt){
    return new Promise(function(resolve){
        var bubble=_findRetryBubble(root);
        if(!bubble){
            _rLog(id+" sticky bubble not found attempt="+attempt);
            resolve({ok:false,error:"sticky_bubble_not_found"});
            return;
        }
        var humanMsg=bubble.querySelector(".composer-human-message")||bubble;
        try{qtClickButton(humanMsg)}catch(e){try{humanMsg.click()}catch(_){}}

        var editorAttempts=0;
        function waitEditor(){
            var editor=bubble.querySelector(_SEL_EDITOR);
            if(editor){waitSendBtn(editor);return}
            editorAttempts++;
            if(editorAttempts>15){
                _rLog(id+" sticky editor timeout attempt="+attempt);
                resolve({ok:false,error:"sticky_editor_timeout"});
                return;
            }
            setTimeout(waitEditor,200);
        }

        function waitSendBtn(editor){
            var btnAttempts=0;
            function tryBtn(){
                var btn=_findSendBtnInBubble(bubble);
                if(btn&&!btn.disabled&&btn.getAttribute("aria-disabled")!=="true"){
                    _rLog(id+" retry submit via sticky bubble attempt="+attempt);
                    qtClickButton(btn);
                    _queuePreviousMessageRevertClicks();
                    resolve({ok:true,method:"sticky_bubble"});
                    return;
                }
                btnAttempts++;
                if(btnAttempts>10){
                    _rLog(id+" sticky send button timeout, Enter fallback attempt="+attempt);
                    qtPressEnter(editor);
                    _queuePreviousMessageRevertClicks();
                    resolve({ok:true,method:"sticky_bubble_enter"});
                    return;
                }
                setTimeout(tryBtn,200);
            }
            tryBtn();
        }
        waitEditor();
    });
}

function _triggerBottomSend(root){
    var btn=_findSendBtn(root);
    if(btn){qtClickButton(btn);return}
    var editor=_findEditor(root);
    if(editor)qtPressEnter(editor);
}

function _retryNormText(v){return String(v||"").trim()}
var _MODEL_REGION_HINT="请更换节点或者开启虚拟网卡";
function _cursorErrorText(v){
    if(!v)return "";
    if(typeof v==="string")return v;
    try{return JSON.stringify(v)}catch(e){try{return String(v)}catch(_){return ""}}
}
function _modelRegionHint(v){
    var text=_cursorErrorText(v);
    var lower=text.toLowerCase();
    if(text.indexOf(_MODEL_REGION_HINT)>=0)return _MODEL_REGION_HINT;
    if(text.indexOf("ERROR_UNSUPPORTED_REGION")>=0)return _MODEL_REGION_HINT;
    if(lower.indexOf("model not available")>=0)return _MODEL_REGION_HINT;
    if(lower.indexOf("not supported in your region")>=0)return _MODEL_REGION_HINT;
    if(lower.indexOf("actionrequired")>=0&&lower.indexOf("change_model")>=0)return _MODEL_REGION_HINT;
    return "";
}
function _fatalCursorHint(v){
    return _modelRegionHint(v);
}
function _readComposerStatus(bridge,composerId,channelId){
    try{return bridge&&bridge.getStatus?bridge.getStatus(composerId,channelId):null}catch(e){return null}
}
function _composerBusy(status){
    if(!status)return false;
    return /generating|applying|reading|running|streaming|thinking|tool|pending/.test(String(status.status||"").toLowerCase());
}
function _composerRetrySuccess(status){
    if(!status||!status.found)return false;
    var state=String(status.status||"").toLowerCase();
    if(state==="generating"||state==="applying")return false;
    if(status.hasError)return false;
    if(_retryNormText(status.lastAiText))return true;
    return /complete|done|finish|success|succeed/.test(state);
}
function _composerRetryWaiting(status){
    if(!status)return false;
    var state=String(status.status||"").toLowerCase();
    if(state==="generating"||state==="applying")return true;
    if(status.hasError)return false;
    return !_retryNormText(status.lastAiText);
}
function _composerProgressSignature(status){
    if(!status)return "missing";
    return [_retryNormText(status.status),_retryNormText(status.lastAiBubbleId),String(Number(status.aiBubbleCount)||0),status.hasError?"error":"ok"].join("|");
}
function _composerStatusDebug(status,inst){
    status=status||{};
    var sinceSubmit=inst&&inst.lastSubmitAt?Date.now()-inst.lastSubmitAt:0;
    var errorText=_retryNormText(status.errorText);
    var runningCheck=status.runningCheckMessages;
    return "status="+(_retryNormText(status.status)||"empty")+
        " found="+(status.found?"1":"0")+
        " busy="+(_composerBusy(status)?"1":"0")+
        " hasError="+(status.hasError?"1":"0")+
        " aiCount="+(Number(status.aiBubbleCount)||0)+
        " aiId="+(_retryNormText(status.lastAiBubbleId)||"")+
        " humanId="+(_retryNormText(status.lastHumanBubbleId)||"")+
        " lastAiLen="+_retryNormText(status.lastAiText).length+
        " sinceSubmit="+sinceSubmit+
        " runningCheck="+(runningCheck&&runningCheck.ok?(_retryNormText(runningCheck.evidence)||"1"):"0")+
        " errorTextLen="+errorText.length+
        " errorText="+errorText;
}
function _runningCheckMessagesEvidence(status,inst){
    var evidence=status&&status.runningCheckMessages;
    if(!evidence||!evidence.ok)return null;
    var targetChannel=_retryNormText(inst&&inst.channelId);
    if(targetChannel&&_retryNormText(evidence.channelId)!==targetChannel)return null;
    return evidence;
}
function _resetSilentNotStarted(inst){
    if(!inst)return;
    inst.silentNotStartedCount=0;
    inst.silentNotStartedSince=0;
    inst.silentNotStartedReason="";
}
function _recordSilentNotStarted(id,inst,status,reason){
    if(!inst||!inst.silentMode)return false;
    var now=Date.now();
    reason=reason||"composer_not_started";
    if(inst.silentNotStartedReason!==reason){
        inst.silentNotStartedReason=reason;
        inst.silentNotStartedCount=0;
        inst.silentNotStartedSince=now;
    }
    inst.silentNotStartedCount=(Number(inst.silentNotStartedCount)||0)+1;
    if(!inst.silentNotStartedSince)inst.silentNotStartedSince=now;
    var age=now-inst.silentNotStartedSince;
    var evidence=reason+":count="+inst.silentNotStartedCount+":age="+age;
    _reportEvidence(id,inst,"retry_waiting",evidence);
    if(inst.silentNotStartedCount>=_SILENT_NOT_STARTED_MAX&&age>=_SILENT_NOT_STARTED_MIN_MS){
        _rWarn("silent composer never started task="+inst.taskId+" composer="+id+" reason="+reason+" count="+inst.silentNotStartedCount+" age="+age+" "+_composerStatusDebug(status,inst));
        inst.running=false;
        _reportDone(id,"failed","composer_never_started_after_submit:"+reason);
        return true;
    }
    return false;
}
function _fatalCursorStatusHint(status){
    if(!status)return "";
    return _fatalCursorHint(status.errorText)||_fatalCursorHint(status);
}
function _failIfFatalCursorStatus(id,inst,reason){
    var bridge=window.__qtComposerBridge;
    if(!inst||!bridge||!bridge.getStatus)return false;
    var status=_readComposerStatus(bridge,id,inst.channelId);
    var fatalHint=_fatalCursorStatusHint(status);
    if(!fatalHint)return false;
    _rWarn("fatal cursor error during "+reason+" task="+inst.taskId+" composer="+id+" "+_composerStatusDebug(status,inst));
    inst.running=false;
    _reportDone(id,"failed",fatalHint);
    return true;
}
function _markRetryBaseline(inst,status){
    status=status||{};
    inst.retryBaselineAiBubbleId=_retryNormText(status.lastAiBubbleId);
    inst.retryBaselineAiBubbleCount=Number(status.aiBubbleCount)||0;
    inst.retryBaselineCaptured=true;
    try{_rLog("baseline captured task="+(inst&&inst.taskId||"")+" composer="+(inst&&inst.composerId||"")+" status="+(_retryNormText(status.status)||"empty")+" aiCount="+inst.retryBaselineAiBubbleCount+" aiId="+inst.retryBaselineAiBubbleId)}catch(e){}
}
function _hasNewAiBubble(inst,status){
    if(!inst||!status||!status.found)return false;
    if(_retryNormText(status.composerId)!==_retryNormText(inst.composerId))return false;
    if(!_retryNormText(status.lastAiText))return false;
    return _hasAiBubbleDelta(inst,status);
}
function _hasAiBubbleDelta(inst,status){
    if(!inst||!status||!status.found)return false;
    if(_retryNormText(status.composerId)!==_retryNormText(inst.composerId))return false;
    var baselineId=_retryNormText(inst.retryBaselineAiBubbleId);
    var currentId=_retryNormText(status.lastAiBubbleId);
    var baselineCount=Number(inst.retryBaselineAiBubbleCount)||0;
    var currentCount=Number(status.aiBubbleCount)||0;
    return currentCount>baselineCount||!!(currentId&&currentId!==baselineId);
}
function _reportEvidence(id,inst,step,evidence){
    _postReport({taskId:inst.taskId,composerId:id,channelId:inst.channelId||"",status:"working",step:step||"confirm_success",retryCount:inst.attempts,evidence:evidence||"",mode:inst.silentMode?"silent":"visible"});
}
function _sessionEvidenceDebug(payload){
    if(!payload)return "sessionEvidence=missing";
    return "sessionEvidence ok="+(payload.ok?"1":"0")+
        " waitingActive="+(payload.waitingActive?"1":"0")+
        " channel="+(payload.channelId||"")+
        " evidence="+(payload.evidence||"");
}
function _sessionEvidenceSignature(payload){
    if(!payload)return "missing";
    return [(payload.ok?"1":"0"),(payload.waitingActive?"1":"0"),payload.channelId||"",payload.evidence||""].join("|");
}
async function _waitSessionSuccessEvidence(id,inst,maxMs,step){
    var deadline=Date.now()+Math.max(500,Number(maxMs)||5000);
    var lastSig="";
    while(_alive(id,inst.generation)&&Date.now()<deadline){
        var sessionEvidence=await _getJson("/auto-chat/session-success?"+_taskQuery(inst));
        var sig=_sessionEvidenceSignature(sessionEvidence);
        if(sig!==lastSig){
            lastSig=sig;
            _rLog("session evidence polling task="+inst.taskId+" composer="+id+" "+_sessionEvidenceDebug(sessionEvidence));
        }
        if(sessionEvidence&&sessionEvidence.ok&&sessionEvidence.evidence){
            inst.running=false;
            _reportDone(id,"done","qingtian:"+sessionEvidence.evidence);
            return true;
        }
        await _retrySleep(id,inst.generation,500);
    }
    _reportEvidence(id,inst,step||"confirm_success","waiting_qingtian_session_success_timeout");
    return false;
}
async function _preserveIfMcpWaiting(id,bridge,inst,step){
    if(!inst)return false;
    var targetChannel=_retryNormText(inst.channelId);
    try{
        var sessionEvidence=await _getJson("/auto-chat/session-success?"+_taskQuery(inst));
        var sessionWaitingActive=sessionEvidence&&sessionEvidence.waitingActive&&(!targetChannel||_retryNormText(sessionEvidence.channelId)===targetChannel);
        if(sessionWaitingActive){
            _resetSilentNotStarted(inst);
            inst.running=false;
            _reportDone(id,"done","qingtian:"+(step||"mcp_waiting_guard")+":session_waiting_active:"+(_retryNormText(sessionEvidence.channelId)||targetChannel));
            return true;
        }
    }catch(e){}
    var status=_readComposerStatus(bridge,id,inst.channelId);
    var runningCheckEvidence=_runningCheckMessagesEvidence(status,inst);
    if(runningCheckEvidence){
        _resetSilentNotStarted(inst);
        inst.running=false;
        _reportDone(id,"done",runningCheckEvidence.evidence||("running_check_messages:qtwx-mcp-"+(inst.channelId||"")));
        return true;
    }
    return false;
}
async function _waitComposerNotBusy(id,bridge,timeoutMs){
    var deadline=Date.now()+timeoutMs;
    while(Date.now()<deadline){
        var status=_readComposerStatus(bridge,id);
        if(!_composerBusy(status))return true;
        await _delay(500);
    }
    return false;
}
async function _stopComposerIfBusy(id,bridge,inst){
    if(await _preserveIfMcpWaiting(id,bridge,inst,"stop_guard")){
        _rLog("diag stopComposerIfBusy preserved MCP waiting "+_retryInstanceDiag(id,inst));
        return false;
    }
    var status=_readComposerStatus(bridge,id,inst&&inst.channelId);
    _rLog("diag stopComposerIfBusy status "+_retryInstanceDiag(id,inst)+" status="+(status&&status.status||"")+" busy="+(_composerBusy(status)?1:0)+" runningCheck="+(status&&status.runningCheckMessages&&status.runningCheckMessages.ok?1:0));
    if(!_composerBusy(status))return true;
    if(!bridge||!bridge.stopByComposerId){
        _reportEvidence(id,inst,"retry_waiting","composer_stop_bridge_missing");
        _rLog("diag stopComposerIfBusy bridge missing "+_retryInstanceDiag(id,inst));
        return false;
    }
    _reportEvidence(id,inst,"stop_running",status.status||"");
    _rLog("diag stopComposerIfBusy calling bridge.stopByComposerId "+_retryInstanceDiag(id,inst)+" status="+(status&&status.status||""));
    try{await bridge.stopByComposerId(id)}catch(e){
        _reportEvidence(id,inst,"retry_waiting",String(e&&e.message||e||"composer_stop_failed"));
        _rLog("diag stopComposerIfBusy stop threw "+_retryInstanceDiag(id,inst)+" error="+String(e&&e.message||e));
        return false;
    }
    if(await _waitComposerNotBusy(id,bridge,_OPTS.COMPOSER_STOP_TIMEOUT_MS)){
        _rLog("diag stopComposerIfBusy stopped ok "+_retryInstanceDiag(id,inst));
        return true;
    }
    _reportEvidence(id,inst,"retry_waiting","composer_stop_timeout");
    _rLog("diag stopComposerIfBusy timeout "+_retryInstanceDiag(id,inst));
    return false;
}
async function _waitSilentComposerSettled(id,bridge,inst){
    var stableSince=null;
    var sessionWaitSince=null;
    var progressSince=Date.now();
    var progressSignature="";
    var sessionEvidenceSignature="";
    var lastSessionEvidence=null;
    var sessionActiveSince=null;
    while(_alive(id,inst.generation)){
        var sessionEvidence=await _getJson("/auto-chat/session-success?"+_taskQuery(inst));
        var currentSessionEvidenceSignature=_sessionEvidenceSignature(sessionEvidence);
        if(currentSessionEvidenceSignature!==sessionEvidenceSignature){
            sessionEvidenceSignature=currentSessionEvidenceSignature;
            lastSessionEvidence=sessionEvidence;
            _rLog("session evidence changed task="+inst.taskId+" composer="+id+" "+_sessionEvidenceDebug(sessionEvidence));
        }
        if(sessionEvidence&&sessionEvidence.ok&&sessionEvidence.evidence){
            inst.running=false;
            _reportDone(id,"done","qingtian:"+sessionEvidence.evidence);
            return "done";
        }
        var targetChannel=_retryNormText(inst.channelId);
        var sessionWaitingActive=sessionEvidence&&sessionEvidence.waitingActive&&(!targetChannel||_retryNormText(sessionEvidence.channelId)===targetChannel);
        if(sessionWaitingActive){
            _resetSilentNotStarted(inst);
            if(sessionActiveSince===null)sessionActiveSince=Date.now();
            var activeFor=Date.now()-sessionActiveSince;
            if(activeFor>=1200){
                inst.running=false;
                _reportDone(id,"done","qingtian:session_waiting_active:"+(_retryNormText(sessionEvidence.channelId)||targetChannel));
                return "done";
            }
            _reportEvidence(id,inst,"confirm_success","session_waiting_active_stable:"+activeFor);
        }else{
            sessionActiveSince=null;
        }
        var status=_readComposerStatus(bridge,id,inst.channelId);
        var runningCheckEvidence=_runningCheckMessagesEvidence(status,inst);
        if(runningCheckEvidence){
            _resetSilentNotStarted(inst);
            inst.running=false;
            _reportDone(id,"done",runningCheckEvidence.evidence||("running_check_messages:qtwx-mcp-"+(inst.channelId||"")));
            return "done";
        }
        var signature=_composerProgressSignature(status);
        if(signature!==progressSignature){
            progressSignature=signature;
            progressSince=Date.now();
            inst.lastWaitStatusSignature=signature;
            inst.lastWaitStatusLogAt=Date.now();
            _rLog("wait status changed task="+inst.taskId+" composer="+id+" attempt="+inst.attempts+" "+_composerStatusDebug(status,inst));
        }else if(!inst.lastWaitStatusLogAt||Date.now()-inst.lastWaitStatusLogAt>=5000){
            inst.lastWaitStatusLogAt=Date.now();
            _rLog("wait status polling task="+inst.taskId+" composer="+id+" attempt="+inst.attempts+" "+_composerStatusDebug(status,inst)+" sinceProgress="+(Date.now()-progressSince));
        }
        var fatalHint=_fatalCursorStatusHint(status);
        if(fatalHint){
            _rWarn("fatal cursor error task="+inst.taskId+" composer="+id+" "+_composerStatusDebug(status,inst));
            inst.running=false;
            _reportDone(id,"failed",fatalHint);
            return "failed";
        }
        var waitingUserAiEvidence=status&&String(status.status||"")==="waitingUser"&&_hasAiBubbleDelta(inst,status);
        if((_hasNewAiBubble(inst,status)&&_composerRetrySuccess(status))||waitingUserAiEvidence){
            _resetSilentNotStarted(inst);
            try{_rLog("evidence candidate task="+inst.taskId+" composer="+id+" status="+(status&&status.status||"")+" aiCount="+(status&&status.aiBubbleCount||0)+" aiId="+(status&&status.lastAiBubbleId||""))}catch(e){}
            if(stableSince===null)stableSince=Date.now();
            var stableFor=Date.now()-stableSince;
            _reportEvidence(id,inst,"confirm_success",(waitingUserAiEvidence?"waiting_user_ai_bubble_stable:":"new_ai_bubble_stable:")+stableFor);
            if(stableFor>=_OPTS.COMPOSER_SUCCESS_STABLE_MS){
                if(sessionWaitSince===null)sessionWaitSince=Date.now();
                if(Date.now()-sessionWaitSince>=(_OPTS.COMPOSER_CONFIRM_SUCCESS_MAX_MS||5000)){
                    _rWarn("session evidence timeout task="+inst.taskId+" composer="+id+" channel="+(inst.channelId||"")+" "+_sessionEvidenceDebug(lastSessionEvidence));
                    _reportEvidence(id,inst,"session_confirm_timeout","ai_bubble_without_qingtian_waiting:"+_sessionEvidenceDebug(lastSessionEvidence));
                    return "again";
                }
                _reportEvidence(id,inst,"confirm_success","waiting_qingtian_session_success:"+(Date.now()-sessionWaitSince)+":"+_sessionEvidenceDebug(lastSessionEvidence));
                await _retrySleep(id,inst.generation,500);
                continue;
            }
            await _retrySleep(id,inst.generation,500);
            continue;
        }
        stableSince=null;
        sessionWaitSince=null;
        if(status&&status.hasError){
            var statusErrorFatalHint=_fatalCursorStatusHint(status);
            if(statusErrorFatalHint){
                _rWarn("fatal model/region error on hasError task="+inst.taskId+" composer="+id+" "+_composerStatusDebug(status,inst));
                inst.running=false;
                _reportDone(id,"failed",statusErrorFatalHint);
                return "failed";
            }
            return "again";
        }
        var composerIsBusy=status&&_composerBusy(status);
        if(composerIsBusy){
            _resetSilentNotStarted(inst);
            var busyFor=Date.now()-progressSince;
            var sinceSubmit=inst.lastSubmitAt?Date.now()-inst.lastSubmitAt:0;
            if(busyFor>=(_OPTS.COMPOSER_STALLED_RETRY_MS||45000)&&(!inst.lastBusyTimeoutSuppressedLogAt||Date.now()-inst.lastBusyTimeoutSuppressedLogAt>=5000)){
                inst.lastBusyTimeoutSuppressedLogAt=Date.now();
                _rLog("diag waitSilentComposerSettled suppress busy timeout task="+inst.taskId+" composer="+id+" attempt="+inst.attempts+" status="+(status&&status.status||"")+" busyFor="+busyFor+" sinceSubmit="+sinceSubmit+" "+_composerStatusDebug(status,inst));
            }
            _reportEvidence(id,inst,"confirm_success","composer_busy_waiting:"+(status.status||"unknown")+":busyFor="+busyFor+":sinceSubmit="+sinceSubmit);
            await _retrySleep(id,inst.generation,_composerRetryWaiting(status)?1500:_OPTS.RETRY_IDLE_MS);
            continue;
        }
        if(status&&!composerIsBusy){
            var notStartedReason=status.found?"composer_not_running_retry":"composer_status_missing_retry";
            if(_recordSilentNotStarted(id,inst,status,notStartedReason))return "failed";
            if(!inst.silentMode)_reportEvidence(id,inst,"retry_waiting",notStartedReason);
            return "again";
        }
        if(inst.lastSubmitAt&&Date.now()-inst.lastSubmitAt>=(_OPTS.COMPOSER_ATTEMPT_RETRY_MS||_OPTS.COMPOSER_STALLED_RETRY_MS)){
            _reportEvidence(id,inst,"retry_waiting","attempt_confirm_timeout:"+(Date.now()-inst.lastSubmitAt));
            return "again";
        }
        _reportEvidence(id,inst,"confirm_success",status&&status.found?"waiting_new_ai_bubble":"composer_status_missing");
        await _retrySleep(id,inst.generation,_composerRetryWaiting(status)?1500:_OPTS.RETRY_IDLE_MS);
    }
    return "stopped";
}
async function _silentRetryLoop(id,gen){
    var inst=_instances[id];
    var bridge=window.__qtComposerBridge;
    if(!inst||!bridge||!bridge.getStatus||!bridge.submitByComposerId){
        _halt(id,"silent_bridge_missing");
        return;
    }
    _rLog("diag silentRetryLoop enter "+_retryInstanceDiag(id,inst));
    if(!inst.retryBaselineCaptured)_markRetryBaseline(inst,_readComposerStatus(bridge,id,inst.channelId));
    inst.attempts=1;
    _reportProgress(id,inst,"silent_wait");
    if(!inst.initialPromptSubmitted){
        var first=await _submitRetryPrompt(id,inst,inst.attempts);
        if(!first||!first.ok){_halt(id,first&&first.error||"silent_submit_failed");return}
        inst.initialPromptSubmitted=true;
        inst.lastSubmitAt=Date.now();
    }else if(!inst.lastSubmitAt){
        inst.lastSubmitAt=Date.now();
    }
    while(_alive(id,gen)&&inst.attempts<(inst.limit||_OPTS.LIMIT)){
        var result=await _waitSilentComposerSettled(id,bridge,inst);
        if(result==="done"||result==="stopped"||result==="failed"){
            _rLog("diag silentRetryLoop exit result="+result+" "+_retryInstanceDiag(id,inst));
            return;
        }
        var stoppedForRetry=await _stopComposerIfBusy(id,bridge,inst);
        if(!_alive(id,gen)){
            _rLog("diag silentRetryLoop exit after stopComposerIfBusy alive=0 "+_retryInstanceDiag(id,inst));
            return;
        }
        if(!stoppedForRetry){
            _rLog("diag silentRetryLoop stopComposerIfBusy returned false "+_retryInstanceDiag(id,inst));
            await _retrySleep(id,gen,_OPTS.RETRY_IDLE_MS);
            continue;
        }
        if(await _preserveIfMcpWaiting(id,bridge,inst,"retry_submit_guard")){
            _rLog("diag silentRetryLoop exit preserved MCP waiting "+_retryInstanceDiag(id,inst));
            return;
        }
        if(!_alive(id,gen)){
            _rLog("diag silentRetryLoop exit before resend alive=0 "+_retryInstanceDiag(id,inst));
            return;
        }
        inst.attempts++;
        _reportProgress(id,inst,"retrying");
        _markRetryBaseline(inst,_readComposerStatus(bridge,id,inst.channelId));
        var resend=await _submitRetryPrompt(id,inst,inst.attempts);
        if(!resend||!resend.ok){
            _halt(id,resend&&resend.error||"silent_retry_submit_failed");
            return;
        }
        inst.lastSubmitAt=Date.now();
        await _retrySleep(id,gen,_OPTS.RETRY_IDLE_MS);
    }
    _rLog("diag silentRetryLoop max/stop exit "+_retryInstanceDiag(id,inst));
    _halt(id,"max_retries");
}

function _initialPromptText(task){
    return String((task&&task.joinPhrase)||(task&&task.text)||"").trim();
}

function _submitInitialPrompt(composerId,task,gen){
    var text=_initialPromptText(task);
    if(!text||task.initialSubmitDone){
        return Promise.resolve({ok:true,skipped:true});
    }
    var bridge=window.__qtComposerBridge;
    var useBridge=task.useComposerBridge!==false;
    _postReport({taskId:task.id,composerId:composerId,status:"working",step:"initial_prompt"});
    if(useBridge&&bridge&&bridge.submitByComposerId){
        _rLog("task "+task.id+" submit initial prompt via bridge len="+text.length);
        return _runSilentLaunchStage("initial_prompt_bridge_submit",_SILENT_INITIAL_BRIDGE_SUBMIT_TIMEOUT_MS,function(){
            return bridge.submitByComposerId(composerId,text,{ignoreQueuing:true});
        }).then(function(stage){
            if(!stage||!stage.ok){
                var stageErr=stage&&stage.error||"bridge_submit_failed";
                if(stageErr==="initial_prompt_bridge_submit_timeout"){
                    _rWarn("task "+task.id+" initial prompt bridge submit timeout; fail instead of entering empty retry loop");
                    return {ok:false,error:"initial_prompt_bridge_submit_timeout"};
                }
                return {ok:false,error:_fatalCursorHint(stageErr)||stageErr};
            }
            var r=stage.value;
            if(r&&r.ok)return {ok:true,method:"bridge"};
            var err=r&&r.error||"bridge_submit_failed";
            return {ok:false,error:_fatalCursorHint(err)||err};
        }).catch(function(e){
            var err=String(e&&e.message||e);
            return {ok:false,error:_fatalCursorHint(err)||err};
        });
    }
    _rLog("task "+task.id+" submit initial prompt via DOM len="+text.length);
    return qtSendPromptToCursor(text,{targetMode:"bound",composerId:composerId,channelId:task.channelId}).then(function(r){
        var err=r&&r.error;
        return {ok:!!(r&&r.ok),method:r&&r.method,error:_fatalCursorHint(err)||err,humanAnchor:r&&r.humanAnchor||null};
    }).catch(function(e){
        var err=String(e&&e.message||e);
        return {ok:false,error:_fatalCursorHint(err)||err};
    });
}

function _submitRetryPrompt(composerId,inst,attempt){
    var bridge=window.__qtComposerBridge;
    if(inst.useComposerBridge!==false&&bridge&&bridge.submitByComposerId){
        var resumeOpts={isResume:true,bubbleId:undefined,ignoreQueuing:true,useEmptyText:true};
        _rLog(composerId+" retry resume via bridge attempt="+attempt);
        return _runSilentLaunchStage("retry_bridge_resume",_SILENT_BRIDGE_RESUME_TIMEOUT_MS,function(){
            return bridge.submitByComposerId(composerId,"",resumeOpts);
        }).then(function(stage){
            if(!stage||!stage.ok){
                var stageErr=stage&&stage.error||"bridge_resume_failed";
                if(stageErr==="retry_bridge_resume_timeout"){
                    _rWarn(composerId+" retry resume bridge timeout attempt="+attempt+"; treat as pending");
                    return {ok:true,method:"bridge_resume_timeout_pending",pending:true};
                }
                return {ok:false,error:_fatalCursorHint(stageErr)||stageErr};
            }
            var r=stage.value;
            if(r&&r.ok){
                _queuePreviousMessageRevertClicks();
                return {ok:true,method:"bridge_resume"};
            }
            _rLog(composerId+" retry resume failed attempt="+attempt+" err="+(r&&r.error||""));
            var err=r&&r.error||"bridge_resume_failed";
            return {ok:false,error:_fatalCursorHint(err)||err};
        }).catch(function(e){
            var err=String(e&&e.message||e);
            return {ok:false,error:_fatalCursorHint(err)||err};
        });
    }
    if(inst&&inst.silentMode){
        _rLog(composerId+" silent retry cannot use visible fallback attempt="+attempt);
        return Promise.resolve({ok:false,error:"silent_bridge_unavailable"});
    }
    var root=_composerRootById(composerId);
    if(!root)return Promise.resolve({ok:false,error:"composer_root_not_found"});
    return _resendFromBubble(root,composerId,attempt);
}

function _startRetryAfterInitialPrompt(composerId,task,gen){
    _submitInitialPrompt(composerId,task,gen).then(function(r){
        if(!_alive(composerId,gen))return;
        if(!r||!r.ok){
            _rLog("task "+task.id+" initial prompt failed: "+(r&&r.error));
            _instances[composerId].running=false;
            _postReport({taskId:task.id,composerId:composerId,status:"failed",step:"initial_prompt_failed",error:r&&r.error});
            return;
        }
        if(r.method||task.initialSubmitDone){
            _instances[composerId].initialPromptSubmitted=true;
        }
        var wait=r.skipped?0:900;
        setTimeout(function(){
            if(_alive(composerId,gen))_retryLoop(composerId,gen);
        },wait);
    }).catch(function(e){
        _instances[composerId].running=false;
        _postReport({taskId:task.id,composerId:composerId,status:"failed",step:"initial_prompt_exception",error:String(e&&e.message||e)});
    });
}

// ─── 重试主循环 ──────────────────────────────────
async function _retryLoop(id,gen){
    var inst=_instances[id];
    if(!inst)return;
    var root=_composerRootById(id);
    if(!root){_halt(id);return}

    inst.attempts=1;_reportProgress(id,inst);
    if(inst.initialPromptSubmitted){
        _rLog(id+" initial prompt already submitted, skip first empty send");
    }else{
        _triggerBottomSend(root);
    }

    // 可见模式兼容旧 DOM 重试路径：先观察目标提示是否出现
    var appeared=await _monitorElement(root,_SEL_POPUP,true,10000);
    if(!_alive(id,gen))return;
    if(_failIfFatalCursorStatus(id,inst,"initial_visible_wait"))return;
    if(!appeared){
        var initialConfirm=await _confirmNoPopupOnObservableComposer(id,"initial_wait");
        if(!_alive(id,gen))return;
        if(_failIfFatalCursorStatus(id,inst,"initial_visible_confirm"))return;
        if(!initialConfirm.ok){_rLog(id+" initial visible wait inconclusive: "+initialConfirm.error);_halt(id);return}
        if(!initialConfirm.hasPopup){
            _rLog(id+" initial visible wait sees no retry blocker on observable composer, success");
            inst.running=false;
            _reportDone(id,"done","visible:no_initial_blocker_after_confirm");
            return;
        }
        root=initialConfirm.root;
    }

    while(_alive(id,gen)&&inst.attempts<(inst.limit||_OPTS.LIMIT)){
        root=_composerRootById(id);
        if(!root){_halt(id);return}
        inst.attempts++;_reportProgress(id,inst);
        await _delay(200);

        var resend=await _submitRetryPrompt(id,inst,inst.attempts);
        if(!resend||!resend.ok){
            _rLog(id+" retry prompt failed attempt="+inst.attempts+": "+(resend&&resend.error));
            var submitFatal=_fatalCursorHint(resend&&resend.error);
            if(submitFatal){_halt(id,submitFatal);return}
            _reportEvidence(id,inst,"retry_waiting","sticky_bubble_submit_failed:"+(resend&&resend.error||"unknown"));
            await _retrySleep(id,gen,_OPTS.RETRY_IDLE_MS);
            continue;
        }

        // 等待阻塞提示消失
        var vanished=await _monitorElement(root,_SEL_POPUP,false,8000);
        if(!vanished)vanished=await _monitorElement(root,_SEL_POPUP,false,20000);
        if(!_alive(id,gen))return;
        if(_failIfFatalCursorStatus(id,inst,"visible_vanish_wait"))return;
        if(!vanished){
            _rLog(id+" visible blocker stuck, continue retry attempts="+inst.attempts);
            _reportEvidence(id,inst,"retry_waiting","visible_blocker_stuck:attempt="+inst.attempts);
            await _retrySleep(id,gen,_OPTS.RETRY_IDLE_MS);
            continue;
        }

        // 稳定消失
        var stable=await _waitSettled(root,_SEL_POPUP,_OPTS.GONE_SETTLE_MS,_OPTS.GONE_SETTLE_CAP);
        if(!_alive(id,gen))return;
        if(_failIfFatalCursorStatus(id,inst,"visible_settle_wait"))return;
        if(!stable){
            _rLog(id+" visible blocker flickering, continue retry attempts="+inst.attempts);
            _reportEvidence(id,inst,"retry_waiting","visible_blocker_flickering:attempt="+inst.attempts);
            await _retrySleep(id,gen,_OPTS.RETRY_IDLE_MS);
            continue;
        }

        // 等阻塞提示是否重新出现
        var back=await _waitStableAppear(root,_SEL_POPUP,10000,_OPTS.APPEAR_SETTLE_MS);
        if(!_alive(id,gen))return;
        if(!back){
            var confirm=await _confirmNoPopupOnObservableComposer(id,"success_confirm");
            if(!_alive(id,gen))return;
            if(_failIfFatalCursorStatus(id,inst,"visible_success_confirm"))return;
            if(!confirm.ok){_rLog(id+" success confirm inconclusive: "+confirm.error);_halt(id);return}
            if(!confirm.hasPopup){
                _rLog(id+" 10s no new visible blocker, target composer observable, success attempts="+inst.attempts);
                inst.running=false;
                _reportDone(id,"done","visible:no_new_blocker_after_10s:attempts="+inst.attempts);
                return;
            }
            _rLog(id+" target composer still has visible blocker, continue retry attempts="+inst.attempts);
            root=confirm.root;
        }
    }
    _halt(id,"max_retries");
}

function _halt(id,reason){
    var inst=_instances[id];
    if(inst){
        var fatalHint=_fatalCursorHint(reason);
        var status=(fatalHint||inst.attempts>=(inst.limit||_OPTS.LIMIT))?"failed":"stopped";
        inst.running=false;
        _reportDone(id,status,fatalHint||reason||"");
    }
}

// ─── 上报 ────────────────────────────────────────
function _reportProgress(id,inst,step){
    _postReport({taskId:inst.taskId,composerId:id,channelId:inst.channelId||"",status:"working",step:step||"retry",retryCount:inst.attempts,mode:inst.silentMode?"silent":"visible"});
}
function _reportDone(id,status,evidence){
    var inst=_instances[id];
    var payload={taskId:inst?inst.taskId:"",composerId:id,channelId:inst&&inst.channelId||"",status:status,step:status,retryCount:inst?inst.attempts:0,mode:inst&&inst.silentMode?"silent":"visible",evidence:evidence||""};
    if(status==="failed"&&evidence)payload.error=evidence;
    if(status==="done"||status==="failed")_postReportReliable(payload,5);
    else _postReport(payload);
}
function _postReport(data){
    try{
        var xhr=new XMLHttpRequest();
        xhr.open("POST","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/cmd?clientId="+encodeURIComponent(qtClientId),true);
        xhr.setRequestHeader("Content-Type","application/json");
        xhr.send(JSON.stringify(data));
    }catch(e){}
}
function _postReportReliable(data,maxAttempts){
    maxAttempts=maxAttempts||5;
    return new Promise(function(resolve){
        var attempt=0;
        function send(){
            attempt++;
            try{
                var xhr=new XMLHttpRequest();
                xhr.open("POST","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/cmd?clientId="+encodeURIComponent(qtClientId),true);
                xhr.setRequestHeader("Content-Type","application/json");
                xhr.timeout=2500;
                xhr.onload=function(){
                    if(xhr.status>=200&&xhr.status<300)return resolve(true);
                    if(attempt>=maxAttempts)return resolve(false);
                    setTimeout(send,250+attempt*250);
                };
                xhr.onerror=function(){
                    if(attempt>=maxAttempts)return resolve(false);
                    setTimeout(send,250+attempt*250);
                };
                xhr.ontimeout=xhr.onerror;
                xhr.send(JSON.stringify(data));
            }catch(e){
                if(attempt>=maxAttempts)return resolve(false);
                setTimeout(send,250+attempt*250);
            }
        }
        send();
    });
}
function _postSilentLaunchProgress(cmd,composerId,channelId,step,evidence){
    try{
        var taskId=String(cmd&&cmd.taskId||"").trim();
        if(!taskId)return;
        _postReport({
            taskId:taskId,
            composerId:composerId||"",
            channelId:String(channelId||cmd.channelId||""),
            status:"waiting",
            step:step||"silent_start",
            mode:"silent",
            evidence:evidence||""
        });
    }catch(e){}
}
function _getJson(path){
    return new Promise(function(resolve){
        try{
            var xhr=new XMLHttpRequest();
            xhr.open("GET","http://127.0.0.1:"+_batchRetryPort+path,true);
            xhr.timeout=2500;
            xhr.onload=function(){
                if(xhr.status===200){
                    try{resolve(JSON.parse(xhr.responseText||"{}"))}catch(e){resolve(null)}
                }else resolve(null);
            };
            xhr.onerror=function(){resolve(null)};
            xhr.ontimeout=function(){resolve(null)};
            xhr.send();
        }catch(e){resolve(null)}
    });
}
function _taskQuery(inst){
    return "taskId="+encodeURIComponent(inst&&inst.taskId||"")+"&channelId="+encodeURIComponent(inst&&inst.channelId||"");
}
async function _waitSilentLaunchWaitingEvidence(cmd,composerId,channelId,timeoutMs){
    var taskId=String(cmd&&cmd.taskId||"").trim();
    channelId=String(channelId||cmd&&cmd.channelId||"").trim();
    if(!taskId&&!channelId)return null;
    var deadline=Date.now()+Math.max(500,Number(timeoutMs)||5000);
    var lastSig="";
    while(Date.now()<deadline){
        var data=await _getJson("/auto-chat/session-success?taskId="+encodeURIComponent(taskId)+"&channelId="+encodeURIComponent(channelId));
        var sig=_sessionEvidenceSignature(data);
        if(sig!==lastSig){
            lastSig=sig;
            _rLog("silent_launch waiting evidence composer="+(composerId||"")+" channel="+(channelId||"")+" "+_sessionEvidenceDebug(data));
        }
        if(data&&data.ok&&data.evidence)return data;
        if(data&&data.waitingActive&&(!channelId||_retryNormText(data.channelId)===_retryNormText(channelId))){
            return {ok:true,evidence:"session_waiting_active:"+(_retryNormText(data.channelId)||channelId)};
        }
        await _delay(500);
    }
    return null;
}
async function _submitByComposerIdWithWaitingEvidence(bridge,composerId,text,cmd,channelId,timeoutMs){
    var before=await _waitSilentLaunchWaitingEvidence(cmd,composerId,channelId,800);
    if(before)return {ok:true,qingtianWaitingEvidence:before.evidence||"qingtian_waiting_active_before_submit"};
    var submitPromise=bridge.submitByComposerId(composerId,text,{ignoreQueuing:true});
    if(!channelId&&!String(cmd&&cmd.taskId||"").trim())return submitPromise;
    var evidencePromise=_waitSilentLaunchWaitingEvidence(cmd,composerId,channelId,timeoutMs).then(function(hit){
        if(hit)return {ok:true,qingtianWaitingEvidence:hit.evidence||"qingtian_waiting_active"};
        return submitPromise;
    });
    return Promise.race([submitPromise,evidencePromise]);
}

// ─── 任务执行 ────────────────────────────────────
function _executeTask(task,isRetry){
    var composerId=task.composerId;
    if(!composerId){_rLog("task "+task.id+" 无 composerId");_postReport({taskId:task.id,status:"failed",step:"no_composer_id"});return}
    var inst=_instances[composerId];
    if(inst&&inst.running){_rLog("task "+task.id+" composer "+composerId+" 已在运行");return}
    _genCounter++;
    var gen=_genCounter;
    _instances[composerId]={
        running:true,
        attempts:0,
        generation:gen,
        taskId:task.id,
        limit:task.maxRetries||_OPTS.LIMIT,
        joinPhrase:_initialPromptText(task),
        channelId:String(task.channelId||""),
        useComposerBridge:task.useComposerBridge!==false,
        composerId:composerId,
        silentMode:task.useComposerBridge!==false,
        lastSubmitAt:0,
        retryBaselineAiBubbleId:"",
        retryBaselineAiBubbleCount:0,
        retryBaselineCaptured:false,
        lastWaitStatusLogAt:0,
        lastWaitStatusSignature:"",
        lastBusyTimeoutSuppressedLogAt:0,
        silentNotStartedCount:0,
        silentNotStartedSince:0,
        silentNotStartedReason:""
    };
    _rLog("开始任务 "+task.id+" composer="+composerId+" retry="+isRetry);

    // 检查 DOM root，没有则先激活 Composer 让其渲染
    if(_instances[composerId].silentMode){
        var silentBridge=window.__qtComposerBridge;
        if(silentBridge&&silentBridge.getStatus)_markRetryBaseline(_instances[composerId],_readComposerStatus(silentBridge,composerId,_instances[composerId].channelId));
        _postReport({taskId:task.id,composerId:composerId,status:"working",step:"silent_start",mode:"silent"});
        _submitInitialPrompt(composerId,task,gen).then(function(r){
            if(!_alive(composerId,gen))return;
            if(!r||!r.ok){
                _instances[composerId].running=false;
                _postReport({taskId:task.id,composerId:composerId,status:"failed",step:"initial_prompt_failed",error:r&&r.error,mode:"silent"});
                return;
            }
            if(r.method||task.initialSubmitDone)_instances[composerId].initialPromptSubmitted=true;
            setTimeout(function(){if(_alive(composerId,gen))_silentRetryLoop(composerId,gen)},r.skipped?0:900);
        }).catch(function(e){
            _instances[composerId].running=false;
            _postReport({taskId:task.id,composerId:composerId,status:"failed",step:"initial_prompt_exception",error:String(e&&e.message||e),mode:"silent"});
        });
        return;
    }

    var root=_composerRootById(composerId);
    if(root){
        _startRetryAfterInitialPrompt(composerId,task,gen);
        return;
    }

    var bridge=window.__qtComposerBridge;
    if(!bridge||!bridge.activateComposer){
        _rLog("task "+task.id+" 无法激活 composer "+composerId+"（bridge 未就绪）");
        _instances[composerId].running=false;
        _postReport({taskId:task.id,composerId:composerId,status:"failed",step:"activate_unavailable"});
        return;
    }

    _postReport({taskId:task.id,composerId:composerId,status:"working",step:"activating"});
    bridge.activateComposer(composerId).then(function(r){
        if(!r||!r.ok){
            _rLog("task "+task.id+" activateComposer failed: "+(r&&r.error));
            _instances[composerId].running=false;
            if(task.channelId)qtClearChannelComposerBinding(task.channelId,composerId);
            _postReport({taskId:task.id,composerId:composerId,status:"failed",step:"activate_failed",error:r&&r.error});
            return;
        }
        _rLog("task "+task.id+" activated via strategy "+r.strategy+", waiting DOM…");
        // 轮询等待 DOM 出现（最多 5s）
        var waited=0;
        var iv=setInterval(function(){
            waited+=200;
            var r2=_composerRootById(composerId);
            if(r2){
                clearInterval(iv);
                _rLog("task "+task.id+" DOM ready after "+waited+"ms, start retryLoop");
                _startRetryAfterInitialPrompt(composerId,task,gen);
            }else if(waited>=5000){
                clearInterval(iv);
                _rLog("task "+task.id+" DOM 5s 未出现，halt");
                _instances[composerId].running=false;
                _postReport({taskId:task.id,composerId:composerId,status:"failed",step:"dom_timeout"});
            }
        },200);
    }).catch(function(e){
        _rLog("task "+task.id+" activateComposer threw: "+e);
        _instances[composerId].running=false;
        _postReport({taskId:task.id,composerId:composerId,status:"failed",step:"activate_exception",error:String(e)});
    });
}

// ─── Silent Launch 执行（客户端侧）──────────────
async function _executeSilentLaunch(cmd){
    var bridge=window.__qtComposerBridge;
    var phase="start";
    var startedAt=Date.now();
    _rLog("silent_launch start replyId="+(cmd.replyId||"")+" sid="+(cmd.sid||"")+" channel="+(cmd.channelId||"")+" visible="+(!!cmd.visibleSubmit));
    if(!bridge||!bridge.ready){
        // 自动触发一次 hook：模拟 Ctrl+N 然后重试
        phase="bridge_wait";
        _rLog("bridge not ready, trying auto-trigger hook…");
        var triggerInfo={attempted:false,clicked:false,error:"",button:null,waitMs:1500};
        try{
            var scan=_findNewAgentCreateButton();
            var btn=scan.target;
            triggerInfo.attempted=true;
            triggerInfo.candidates=(scan.candidates||[]).slice(0,8);
            triggerInfo.button=_qtDiagElement(btn);
            if(btn){
                btn.click();
                triggerInfo.clicked=true;
                _rLog("bridge auto-trigger clicked new agent target="+_silentLaunchDetailsText(triggerInfo.button));
                await _delay(1500);
            }else{
                _rLog("bridge auto-trigger new agent button not found candidates="+_silentLaunchDetailsText(triggerInfo.candidates||[]));
            }
        }catch(e){triggerInfo.error=String(e&&e.message||e)}
        bridge=window.__qtComposerBridge;
        if(!bridge||!bridge.ready){
            var diag=_logComposerBridgeDiag("composer_bridge_unavailable",phase,cmd,triggerInfo);
            _postSilentLaunchResult(cmd.replyId,false,null,"composer_bridge_unavailable","",false,false,phase,Date.now()-startedAt,diag);
            return;
        }
    }
    if(!bridge.createAgent){
        var createDiag=_logComposerBridgeDiag("create_agent_unavailable","bridge_ready",cmd,null);
        _postSilentLaunchResult(cmd.replyId,false,null,"create_agent_unavailable","",false,false,"bridge_ready",Date.now()-startedAt,createDiag);
        return;
    }
    try{
        var excluded=cmd.excludedComposerIds||[];
        var excludeSet=new Set(excluded);
        var channelId=String(cmd.channelId||"").trim();
        var visibleSubmit=!!cmd.visibleSubmit;

        // === 复用已有 Composer ===
        if(cmd.reuseExisting){
            phase="list_existing";
            var existing=bridge.listComposers()||[];
            _rLog("silent_launch existing count="+existing.length+" excluded="+excluded.length);
            for(var i=0;i<existing.length;i++){
                var c=existing[i];
                if(c.composerId&&!excludeSet.has(c.composerId)){
                    _rLog("reuse existing composer: "+c.composerId+" name="+c.name);
                    // 改名标记为当前 sid
                    [0,3000].forEach(function(ms){
                        setTimeout(function(){
                            try{bridge.renameComposer(c.composerId,cmd.sid||cmd.name)}catch(e){}
                        },ms);
                    });
                    if(channelId)qtBindChannelComposer(channelId,c.composerId,"auto_reuse_existing");
                    _postSilentLaunchResult(cmd.replyId,true,c.composerId,null,false,true,false,"reuse_existing",Date.now()-startedAt);
                    return;
                }
            }
            _rLog("no reusable composer found, creating new one");
        }

        // === 新建 Composer ===
        phase="pre_list";
        var preIds=new Set((bridge.listComposers()||[]).map(function(c){return c.composerId}));
        var preTs=Date.now();
        _rLog("silent_launch create begin preCount="+preIds.size+" name="+(cmd.name||cmd.sid||""));
        phase="create_agent";
        var createStage=await _runSilentLaunchStage("create_agent",_SILENT_CREATE_TIMEOUT_MS,function(){
            return bridge.createAgent("",cmd.name||cmd.sid,{
                skipShowAndFocus:!visibleSubmit,skipFocus:!visibleSubmit,skipSelect:!visibleSubmit,autoSubmit:false
            });
        });
        if(!createStage.ok){
            _postSilentLaunchResult(cmd.replyId,false,null,createStage.error||"create_agent_failed",false,false,false,phase,Date.now()-startedAt);
            return;
        }
        var created=createStage.value;
        _rLog("silent_launch create returned "+_silentLaunchDetailsText(created));
        phase="reconcile";
        var composerId=created&&created.composerId;
        var reconciled=false;

        // 对账：部分 Cursor 版本 createComposer 可见启动会返回空，需要从列表差异恢复 composerId。
        var postList=bridge.listComposers?bridge.listComposers()||[]:[];
        _rLog("silent_launch postList count="+postList.length+" createdComposer="+(composerId||""));
        if(!composerId){
            var diff=postList.filter(function(c){return c&&c.composerId&&!preIds.has(c.composerId)});
            if(diff.length>0){
                composerId=diff[diff.length-1].composerId;
                reconciled=true;
            }
        }
        if((!composerId||(preIds.has(composerId)||!postList.find(function(c){return c.composerId===composerId})))&&bridge.getRecentlyCreatedComposers){
            var recent=bridge.getRecentlyCreatedComposers(Date.now()-preTs+5000)||[];
            var match=recent.find(function(c){return c&&c.composerId&&!preIds.has(c.composerId)});
            if(match){composerId=match.composerId;reconciled=true}
        }
        if(!composerId&&visibleSubmit){
            await _delay(800);
            postList=bridge.listComposers?bridge.listComposers()||[]:[];
            var delayedDiff=postList.filter(function(c){return c&&c.composerId&&!preIds.has(c.composerId)});
            if(delayedDiff.length>0){
                composerId=delayedDiff[delayedDiff.length-1].composerId;
                reconciled=true;
            }
        }

        if(!composerId){
            _postSilentLaunchResult(cmd.replyId,false,null,created&&created.error||"no_composer_id_returned",reconciled,false,false,phase,Date.now()-startedAt);
            return;
        }
        if(channelId)qtBindChannelComposer(channelId,composerId,"auto_silent_launch");
        _postSilentLaunchProgress(cmd,composerId,channelId,cmd.joinPhrase?"initial_prompt":"waiting_dispatch");

        // 提交 join phrase
        var initialSubmitted=false;
        if(cmd.joinPhrase){
            if(visibleSubmit){
                phase="visible_submit_wait_root";
                var waited=0;
                while(!_composerRootById(composerId)&&waited<5000){
                    await _delay(200);
                    waited+=200;
                }
                phase="visible_submit";
                var visibleStage=await _runSilentLaunchStage("visible_submit",_SILENT_VISIBLE_SUBMIT_TIMEOUT_MS,function(){
                    return qtSendPromptToCursor(cmd.joinPhrase,{targetMode:"bound",composerId:composerId,channelId:channelId});
                });
                if(!visibleStage.ok){
                    _postSilentLaunchResult(cmd.replyId,false,composerId,visibleStage.error||"visible_submit_failed",reconciled,false,false,phase,Date.now()-startedAt);
                    return;
                }
                var submitResult=visibleStage.value;
                if(!submitResult||!submitResult.ok){
                    _postSilentLaunchResult(cmd.replyId,false,composerId,submitResult&&submitResult.error||"visible_submit_failed",reconciled,false,false,phase,Date.now()-startedAt);
                    return;
                }
            }else{
                phase="bridge_submit";
                var bridgeStage=await _runSilentLaunchStage("bridge_submit",_SILENT_INITIAL_BRIDGE_SUBMIT_TIMEOUT_MS,function(){
                    return _submitByComposerIdWithWaitingEvidence(bridge,composerId,cmd.joinPhrase,cmd,channelId,_SILENT_INITIAL_BRIDGE_SUBMIT_TIMEOUT_MS);
                });
                if(!bridgeStage.ok){
                    if(bridgeStage.error==="bridge_submit_timeout"){
                        _rWarn("silent_launch bridge_submit_timeout fail composer="+composerId);
                        _postSilentLaunchResult(cmd.replyId,false,composerId,"initial_prompt_bridge_submit_timeout",reconciled,false,false,phase,Date.now()-startedAt,{stage:bridgeStage});
                        return;
                    }else{
                        _postSilentLaunchResult(cmd.replyId,false,composerId,bridgeStage.error||"bridge_submit_failed",reconciled,false,false,phase,Date.now()-startedAt);
                        return;
                    }
                }else{
                    var bridgeResult=bridgeStage.value;
                    if(!bridgeResult||!bridgeResult.ok){
                        _postSilentLaunchResult(cmd.replyId,false,composerId,bridgeResult&&bridgeResult.error||"bridge_submit_failed",reconciled,false,false,phase,Date.now()-startedAt);
                        return;
                    }
                    if(bridgeResult.qingtianWaitingEvidence){
                        _rLog("silent_launch bridge_submit satisfied by waiting evidence composer="+composerId+" evidence="+bridgeResult.qingtianWaitingEvidence);
                    }
                }
            }
            initialSubmitted=true;
            _postSilentLaunchProgress(cmd,composerId,channelId,"waiting_dispatch");
        }

        // 多次强制改名（对抗 Cursor 自动命名）
        [0,3000,8000,15000].forEach(function(ms){
            setTimeout(function(){
                try{bridge.renameComposer(composerId,cmd.sid||cmd.name)}catch(e){}
            },ms);
        });

        _postSilentLaunchResult(cmd.replyId,true,composerId,null,reconciled,false,initialSubmitted,"done",Date.now()-startedAt);
    }catch(e){
        _rLog("silent_launch exception phase="+phase+" error="+String(e&&e.stack||e&&e.message||e));
        _postSilentLaunchResult(cmd.replyId,false,null,String(e&&e.message||e),false,false,false,phase,Date.now()-startedAt);
    }
}

function _postSilentLaunchResult(replyId,ok,composerId,error,reconciled,reused,initialSubmitted,phase,elapsedMs,details){
    try{
        _rLog("silent_launch result ok="+ok+" composer="+(composerId||"")+" phase="+(phase||"")+" elapsed="+(elapsedMs||0)+" error="+(error||""));
        var xhr=new XMLHttpRequest();
        xhr.open("POST","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/silent-launch-result",true);
        xhr.setRequestHeader("Content-Type","application/json");
        xhr.send(JSON.stringify({replyId:replyId,ok:ok,composerId:composerId||"",clientId:qtClientId,error:error||"",reconciled:!!reconciled,reused:!!reused,initialSubmitted:!!initialSubmitted,phase:phase||"",elapsedMs:Number(elapsedMs)||0,details:details||null}));
    }catch(e){}
}

function _postComposerListResult(reqId,ok,composers,error,details){
    try{
        var xhr=new XMLHttpRequest();
        xhr.open("POST","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/composer-list-result",true);
        xhr.setRequestHeader("Content-Type","application/json");
        xhr.send(JSON.stringify({reqId:reqId,ok:!!ok,composers:composers||[],clientId:qtClientId,error:error||"",details:details||null}));
    }catch(e){}
}

function _executeListComposers(cmd){
    var bridge=window.__qtComposerBridge;
    if(!bridge||!bridge.listComposers){
        var diag=_logComposerBridgeDiag("composer_bridge_unavailable","list_composers",cmd,null);
        _postComposerListResult(cmd.reqId,false,[],"composer_bridge_unavailable",diag);
        return;
    }
    try{
        _postComposerListResult(cmd.reqId,true,bridge.listComposers()||[],"");
    }catch(e){
        _postComposerListResult(cmd.reqId,false,[],String(e&&e.message||e));
    }
}

function _postChannelBindingsResult(reqId,ok,bindings,error,allBindings,occupiedChannels){
    try{
        var xhr=new XMLHttpRequest();
        xhr.open("POST","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/channel-bindings-result",true);
        xhr.setRequestHeader("Content-Type","application/json");
        xhr.send(JSON.stringify({reqId:reqId,ok:!!ok,bindings:bindings||[],allBindings:allBindings||[],occupiedChannels:occupiedChannels||[],clientId:qtClientId,error:error||""}));
    }catch(e){}
}

function _executeListChannelBindings(cmd){
    try{
        var composersById={};
        var bridge=window.__qtComposerBridge;
        if(bridge&&bridge.listComposers){
            var composers=bridge.listComposers()||[];
            for(var i=0;i<composers.length;i++)composersById[composers[i].composerId]=composers[i];
        }
        var bottomButtonState=qtReadBottomComposerButtonState();
        var detectedChannelBindings=qtListCheckMessagesChannelComposerBindings();
        var occupiedChannels=qtListRunningCheckMessagesChannels();
        var allBindings=qtReadChannelComposerBindings().map(function(binding){
            var ownerClientId=String(binding.clientId||"").trim();
            var composer=composersById[binding.composerId];
            var root=qtFindComposerRootById(binding.composerId);
            var data=null;
            try{
                var bridge=window.__qtComposerBridge;
                data=bridge&&bridge.getComposerData?bridge.getComposerData(binding.composerId):null;
            }catch(e){}
            var loaded=!!(composer||root||data);
            var attached=!!root;
            var observable=!!(root&&qtVisibleAny(root));
            var activeRoot=document.activeElement&&document.activeElement.closest?document.activeElement.closest("[data-composer-id]"):null;
            var activeComposerId=activeRoot?qtComposerIdFromRoot(activeRoot):"";
            var clientMatches=!!ownerClientId&&ownerClientId===qtClientId;
            var activeComposerMatches=!!activeComposerId&&activeComposerId===binding.composerId;
            var checkState=qtReadCheckMessagesToolState(binding.channelId,root,false);
            var runningCheckActive=!!(checkState&&checkState.running);
            var visibleGeneratingActive=!!(
                clientMatches&&
                attached&&
                observable&&
                bottomButtonState.hasStopButton===true&&
                bottomButtonState.hasMicButton!==true
            );
            var connectedActive=runningCheckActive||visibleGeneratingActive;
            return Object.assign({},binding,{
                clientId:ownerClientId||qtClientId,
                exists:loaded,
                loaded:loaded,
                attached:attached,
                observable:observable,
                activeComposerId:activeComposerId,
                clientMatches:clientMatches,
                activeComposerMatches:activeComposerMatches,
                runningCheckActive:runningCheckActive,
                visibleGeneratingActive:visibleGeneratingActive,
                connectedActive:connectedActive,
                connectionReason:runningCheckActive?"running_check_messages":(visibleGeneratingActive?"visible_stop_button":""),
                checkMessagesStatus:checkState&&checkState.status||"",
                checkMessagesEvidence:checkState&&checkState.evidence||"",
                checkMessagesSource:checkState&&checkState.source||"",
                bottomStopButton:bottomButtonState.hasStopButton===true,
                bottomMicButton:bottomButtonState.hasMicButton===true,
                name:composer&&composer.name||data&&data.name||""
            });
        });
        if(detectedChannelBindings&&detectedChannelBindings.length){
            var byChannel={};
            for(var d0=0;d0<allBindings.length;d0++){
                var existingChannelId=String(allBindings[d0]&&allBindings[d0].channelId||"").trim();
                if(existingChannelId)byChannel[existingChannelId]=allBindings[d0];
            }
            for(var d1=0;d1<detectedChannelBindings.length;d1++){
                var detected=detectedChannelBindings[d1];
                var detectedChannelId=String(detected&&detected.channelId||"").trim();
                var detectedComposerId=String(detected&&detected.composerId||"").trim();
                if(!detectedChannelId||!detectedComposerId)continue;
                var previous=byChannel[detectedChannelId]||{};
                byChannel[detectedChannelId]=Object.assign({},previous,detected);
            }
            allBindings=[];
            for(var dKey in byChannel){
                if(Object.prototype.hasOwnProperty.call(byChannel,dKey))allBindings.push(byChannel[dKey]);
            }
        }
        var bindings=allBindings.filter(function(binding){
            return binding&&
                binding.clientMatches===true&&
                binding.exists===true&&
                binding.observable===true&&
                binding.connectedActive===true;
        });
        _postChannelBindingsResult(cmd.reqId,true,bindings,"",allBindings,occupiedChannels);
    }catch(e){
        _postChannelBindingsResult(cmd.reqId,false,[],String(e&&e.message||e));
    }
}

function _postChannelBindResult(reqId,ok,binding,error){
    try{
        var xhr=new XMLHttpRequest();
        xhr.open("POST","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/channel-bind-result",true);
        xhr.setRequestHeader("Content-Type","application/json");
        xhr.send(JSON.stringify({reqId:reqId,ok:!!ok,binding:binding||null,clientId:qtClientId,error:error||""}));
    }catch(e){}
}

function _executeBindActiveChannel(cmd){
    try{
        var channelId=String(cmd.channelId||"").trim();
        if(!channelId){_postChannelBindResult(cmd.reqId,false,null,"missing_channelId");return}
        var focused=false;
        try{focused=!!qtWindowFocused()}catch(e){}
        if(!focused&&cmd.allowUnfocused!==true){
            _postChannelBindResult(cmd.reqId,false,{channelId:channelId,clientId:qtClientId,focused:focused,skipped:true},"client_not_focused");
            return;
        }
        var input=qtFindComposerInput();
        var composerId=qtComposerIdFromInput(input);
        var bridge=window.__qtComposerBridge;
        if(!composerId){
            var list=bridge&&bridge.listComposers?bridge.listComposers():[];
            if(list&&list.length===1)composerId=list[0].composerId;
        }
        if(!composerId){_postChannelBindResult(cmd.reqId,false,null,"active_composer_not_found");return}
        qtBindChannelComposer(channelId,composerId,"manual");
        var root=qtFindComposerRootById(composerId);
        var attached=!!root;
        var composer=null;
        var data=null;
        try{
            var list2=bridge&&bridge.listComposers?bridge.listComposers():[];
            composer=(list2||[]).find(function(c){return c&&c.composerId===composerId})||null;
            data=bridge&&bridge.getComposerData?bridge.getComposerData(composerId):null;
        }catch(e){}
        var loaded=!!(composer||root||data);
        var observable=!!(root&&qtVisibleAny(root));
        _postChannelBindResult(cmd.reqId,true,{channelId:channelId,composerId:composerId,clientId:qtClientId,updatedAt:Date.now(),exists:loaded,loaded:loaded,attached:attached,observable:observable,focused:focused,clientMatches:true,activeComposerMatches:true,activeComposerId:composerId},"");
    }catch(e){
        _postChannelBindResult(cmd.reqId,false,null,String(e&&e.message||e));
    }
}

function _executeClearChannelBinding(cmd){
    try{
        var channelId=String(cmd.channelId||"").trim();
        var composerId=String(cmd.composerId||"").trim();
        if(!channelId){_postChannelBindResult(cmd.reqId,false,null,"missing_channelId");return}
        var cleared=qtClearChannelComposerBinding(channelId,composerId);
        _postChannelBindResult(cmd.reqId,true,{channelId:channelId,composerId:composerId,clientId:qtClientId,updatedAt:Date.now(),exists:false,cleared:cleared},"");
    }catch(e){
        _postChannelBindResult(cmd.reqId,false,null,String(e&&e.message||e));
    }
}

function _postActivateComposerResult(cmd,ok,error){
    _rLog("activate_composer result reqId="+(cmd.reqId||"")+" task="+(cmd.taskId||"")+" composer="+(cmd.composerId||"")+" ok="+(!!ok)+" error="+(error||""));
    try{
        var xhr=new XMLHttpRequest();
        xhr.open("POST","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/activate-composer-result",true);
        xhr.setRequestHeader("Content-Type","application/json");
        xhr.send(JSON.stringify({reqId:cmd.reqId||"",taskId:cmd.taskId||"",composerId:cmd.composerId||"",clientId:qtClientId,ok:!!ok,error:error||""}));
    }catch(e){}
}

function _executeActivateComposer(cmd){
    var composerId=String(cmd.composerId||"").trim();
    _rLog("activate_composer received reqId="+(cmd.reqId||"")+" task="+(cmd.taskId||"")+" composer="+composerId+" client="+qtClientId+" port="+_batchRetryPort);
    if(!composerId){
        _postActivateComposerResult(cmd,false,"missing_composerId");
        return;
    }
    var bridge=window.__qtComposerBridge;
    if(!bridge||!bridge.activateComposer){
        _rLog("activate_composer bridge unavailable hasBridge="+(!!bridge)+" hasActivate="+(!!(bridge&&bridge.activateComposer)));
        _postActivateComposerResult(cmd,false,"activate_unavailable");
        return;
    }
    _rLog("activate_composer bridge call begin reqId="+(cmd.reqId||"")+" composer="+composerId);
    bridge.activateComposer(composerId).then(function(r){
        _rLog("activate_composer bridge call result reqId="+(cmd.reqId||"")+" composer="+composerId+" result="+JSON.stringify(r||{}));
        if(r&&r.ok){
            _postActivateComposerResult(cmd,true,"");
        }else{
            _postActivateComposerResult(cmd,false,r&&r.error||"activate_failed");
        }
    }).catch(function(e){
        _rLog("activate_composer bridge call threw reqId="+(cmd.reqId||"")+" composer="+composerId+" error="+String(e&&e.stack||e&&e.message||e));
        _postActivateComposerResult(cmd,false,String(e&&e.message||e));
    });
}

// ─── Watch 机制（自动续发）───────────────────────
function _activeComposerId(){
    try{
        var focused=document.activeElement&&document.activeElement.closest?document.activeElement.closest("[data-composer-id]"):null;
        if(focused&&qtVisibleAny(focused))return String(focused.getAttribute("data-composer-id")||"");
    }catch(e){}
    try{
        var roots=Array.prototype.slice.call(document.querySelectorAll("[data-composer-id]")).filter(qtVisibleAny);
        if(roots.length)return String(roots[0].getAttribute("data-composer-id")||"");
    }catch(e){}
    return "";
}

function _describeRevealElement(el,index){
    return {
        i:index,
        text:String((el&&el.innerText||el&&el.textContent||"")).trim().slice(0,120),
        aria:String(el&&el.getAttribute&&el.getAttribute("aria-label")||"").slice(0,160),
        title:String(el&&el.getAttribute&&el.getAttribute("title")||"").slice(0,160),
        cls:String(el&&el.className||"").slice(0,200),
        role:String(el&&el.getAttribute&&el.getAttribute("role")||"").slice(0,80),
        tag:String(el&&el.tagName||""),
        disabled:!!(el&&el.disabled),
        visible:!!(el&&qtVisibleAny(el))
    };
}

function _scoreNewAgentCreateButton(el){
    if(!el||!qtVisibleAny(el))return -1;
    var tag=String(el.tagName||"").toUpperCase();
    var text=String(el.innerText||el.textContent||"");
    var aria=String(el.getAttribute&&el.getAttribute("aria-label")||"");
    var title=String(el.getAttribute&&el.getAttribute("title")||"");
    var cls=String(el.className||"");
    var role=String(el.getAttribute&&el.getAttribute("role")||"").toLowerCase();
    var hay=(text+" "+aria+" "+title+" "+cls).toLowerCase();
    if(role==="tab")return -1;
    if((el.disabled||el.getAttribute&&el.getAttribute("aria-disabled")==="true"))return -1;
    var looksButton=role==="button"||tag==="BUTTON"||tag==="A"||hay.indexOf("action-label")>=0||hay.indexOf("action-item")>=0||hay.indexOf("codicon-")>=0;
    if(!looksButton)return -1;
    var score=0;
    if(hay.indexOf("new agent")>=0)score+=70;
    if(hay.indexOf("new chat")>=0)score+=45;
    if(hay.indexOf("new composer")>=0)score+=45;
    if(hay.indexOf("ctrl+n")>=0)score+=90;
    if(hay.indexOf("replace agent")>=0)score+=80;
    if(hay.indexOf("codicon-add-two")>=0)score+=120;
    if(hay.indexOf("codicon-add")>=0)score+=30;
    if(hay.indexOf("action-label")>=0)score+=35;
    if(role==="button")score+=35;
    if(tag==="A")score+=20;
    if(tag==="BUTTON")score+=20;
    if(hay.indexOf("tab")>=0&&hay.indexOf("codicon-add")<0)score-=80;
    return score>0?score:-1;
}

function _findNewAgentCreateButton(){
    var found=[];
    try{
        var nodes=Array.prototype.slice.call(document.querySelectorAll('a.action-label.codicon-add-two,a.codicon-add-two,button.codicon-add-two,[role="button"].codicon-add-two,button,[role="button"],a.action-label,.action-label,.action-item,.monaco-toolbar *'));
        var seen=[];
        for(var i=0;i<nodes.length;i++){
            var el=nodes[i];
            if(seen.indexOf(el)>=0)continue;
            seen.push(el);
            var score=_scoreNewAgentCreateButton(el);
            if(score<0)continue;
            found.push({el:el,score:score,info:_describeRevealElement(el,i)});
        }
    }catch(e){
        _rLog("new_agent_button scan error="+String(e&&e.message||e));
    }
    found.sort(function(a,b){return b.score-a.score});
    return {target:found.length?found[0].el:null,candidates:found.map(function(x){var info=x.info;info.score=x.score;return info})};
}

function _findNewAgentButtonForReveal(){
    return _findNewAgentCreateButton();
}

function _waitForActiveComposerChange(beforeId,timeoutMs){
    var started=Date.now();
    return new Promise(function(resolve){
        function tick(){
            var current=_activeComposerId();
            if(current&&current!==beforeId){
                resolve({ok:true,composerId:current,elapsedMs:Date.now()-started});
                return;
            }
            if(Date.now()-started>=timeoutMs){
                resolve({ok:false,composerId:current,elapsedMs:Date.now()-started});
                return;
            }
            setTimeout(tick,120);
        }
        tick();
    });
}

function _waitForComposerVisible(composerId,timeoutMs){
    var started=Date.now();
    return new Promise(function(resolve){
        function tick(){
            var root=_composerRootById(composerId);
            var visible=!!(root&&qtVisibleAny(root));
            if(visible){
                resolve({ok:true,visible:true,elapsedMs:Date.now()-started});
                return;
            }
            if(Date.now()-started>=timeoutMs){
                resolve({ok:false,visible:false,elapsedMs:Date.now()-started});
                return;
            }
            setTimeout(tick,120);
        }
        tick();
    });
}

function _executeRevealComposerInNewAgent(cmd){
    var composerId=String(cmd.composerId||"").trim();
    var started=Date.now();
    _rLog("reveal_composer_in_new_agent received reqId="+(cmd.reqId||"")+" task="+(cmd.taskId||"")+" composer="+composerId+" client="+qtClientId);
    if(!composerId){
        _postActivateComposerResult(cmd,false,"missing_composerId");
        return;
    }
    var bridge=window.__qtComposerBridge;
    if(!bridge||!bridge.activateComposer){
        _rLog("reveal_composer_in_new_agent bridge unavailable hasBridge="+(!!bridge)+" hasActivate="+(!!(bridge&&bridge.activateComposer)));
        _postActivateComposerResult(cmd,false,"activate_unavailable");
        return;
    }
    var beforeId=_activeComposerId();
    var scan=_findNewAgentButtonForReveal();
    _rLog("reveal_composer_in_new_agent begin reqId="+(cmd.reqId||"")+" beforeActive="+beforeId+" candidates="+_silentLaunchDetailsText((scan.candidates||[]).slice(0,8)));
    if(!scan.target){
        _postActivateComposerResult(cmd,false,"new_agent_button_not_found");
        return;
    }
    try{
        scan.target.click();
        _rLog("reveal_composer_in_new_agent clicked new agent reqId="+(cmd.reqId||"")+" target="+_silentLaunchDetailsText(_describeRevealElement(scan.target,0)));
    }catch(e){
        var clickError=String(e&&e.message||e);
        _rLog("reveal_composer_in_new_agent click failed reqId="+(cmd.reqId||"")+" error="+clickError);
        _postActivateComposerResult(cmd,false,"new_agent_click_failed:"+clickError);
        return;
    }
    _waitForActiveComposerChange(beforeId,4500).then(function(change){
        _rLog("reveal_composer_in_new_agent after new agent reqId="+(cmd.reqId||"")+" result="+_silentLaunchDetailsText(change));
        if(!change||!change.ok){
            _postActivateComposerResult(cmd,false,"new_agent_not_opened");
            return null;
        }
        _rLog("reveal_composer_in_new_agent activate begin reqId="+(cmd.reqId||"")+" target="+composerId+" newAgent="+(change.composerId||""));
        return bridge.activateComposer(composerId).then(function(result){
            _rLog("reveal_composer_in_new_agent activate result reqId="+(cmd.reqId||"")+" result="+_silentLaunchDetailsText(result));
            if(!result||!result.ok){
                _postActivateComposerResult(cmd,false,result&&result.error||"activate_failed");
                return null;
            }
            return _waitForComposerVisible(composerId,3500).then(function(visible){
                var finalId=_activeComposerId();
                _rLog("reveal_composer_in_new_agent final reqId="+(cmd.reqId||"")+" target="+composerId+" finalActive="+finalId+" visible="+_silentLaunchDetailsText(visible)+" elapsed="+(Date.now()-started));
                if(visible&&visible.ok){
                    _postActivateComposerResult(cmd,true,"");
                }else{
                    _postActivateComposerResult(cmd,false,"target_not_visible_after_activate");
                }
            });
        });
    }).catch(function(e){
        var error=String(e&&e.stack||e&&e.message||e);
        _rLog("reveal_composer_in_new_agent threw reqId="+(cmd.reqId||"")+" error="+error);
        _postActivateComposerResult(cmd,false,error);
    });
}

function _processWatchSet(cmd){
    _watchVersion=cmd.version||0;
    _watchedMap={};
    var entries=cmd.entries||[];
    for(var i=0;i<entries.length;i++){
        var e=entries[i];
        if(e.composerId){
            _watchedMap[e.composerId]={sid:e.sid,throttleMs:Math.max(500,e.throttleMs||3000),lastCheck:0};
        }
    }
    _rLog("watch set updated v="+_watchVersion+" count="+entries.length);
}

function _watchTick(){
    var now=Date.now();
    for(var cid in _watchedMap){
        var w=_watchedMap[cid];
        if(now-w.lastCheck<w.throttleMs)continue;
        w.lastCheck=now;
        var root=_composerRootById(cid);
        if(!root)continue;
        var popup=_findPopup(root);
        if(popup){
            _rLog("watch: popup detected on "+cid+", auto-resend");
            _resendFromBubble(root,cid,0);
        }
    }
}
_qtSetInterval(_watchTick,1000);

// ─── 主轮询循环 ──────────────────────────────────
var _pollBusy=false;
var _cmdPollFailures=0;
function _cmdPollDelay(){
    if(_cmdPollFailures<=0)return 260;
    return Math.min(30000,1000*Math.pow(2,Math.min(_cmdPollFailures-1,5)));
}
function _cmdPollOk(){_cmdPollFailures=0}
function _cmdPollFailed(){
    _cmdPollFailures=Math.min(_cmdPollFailures+1,6);
    if(_cmdPollFailures>=2)_discoverBatchRetryPort(null,false);
}
function _schedulePollCmd(ms){
    if(_qtStopped)return;
    _qtSetTimeout(_pollCmd,typeof ms==="number"?ms:_cmdPollDelay());
}
function _pollCmd(){
    if(_qtStopped)return;
    if(_aborted||_pollBusy){_schedulePollCmd(500);return}
    _pollBusy=true;
    var xhr=new XMLHttpRequest();
    xhr.open("GET","http://127.0.0.1:"+_batchRetryPort+"/auto-chat/cmd?clientId="+encodeURIComponent(qtClientId),true);
    xhr.timeout=3000;
    xhr.onload=function(){
        _pollBusy=false;
        if(xhr.status>=200&&xhr.status<300)_cmdPollOk();else _cmdPollFailed();
        if(xhr.status===204)_discoverBatchRetryPort(null,false);
        if(xhr.status===200&&xhr.responseText){
            try{
                var cmd=JSON.parse(xhr.responseText);
                if(cmd)_dispatchCmd(cmd);
            }catch(e){}
        }
        _schedulePollCmd();
    };
    xhr.onerror=function(){_pollBusy=false;_cmdPollFailed();_schedulePollCmd()};
    xhr.ontimeout=function(){_pollBusy=false;_cmdPollFailed();_schedulePollCmd()};
    try{xhr.send()}catch(e){_pollBusy=false;_cmdPollFailed();_schedulePollCmd()}
}

function _dispatchCmd(cmd){
    if(cmd&&cmd.workspaceScopeId)qtSetBatchWorkspaceScopeId(cmd.workspaceScopeId,"cmd:"+String(cmd.action||""));
    switch(cmd.action){
        case "batch":
            var tasks=cmd.tasks||[];
            for(var i=0;i<tasks.length;i++){
                var t=tasks[i];
                _executeTask(t,t.action==="retry"||t.action==="send_with_retry");
            }
            break;
        case "silent_launch":
            _executeSilentLaunch(cmd);
            break;
        case "list_composers":
            _executeListComposers(cmd);
            break;
        case "list_channel_bindings":
            _executeListChannelBindings(cmd);
            break;
        case "bind_active_channel":
            _executeBindActiveChannel(cmd);
            break;
        case "clear_channel_binding":
            _executeClearChannelBinding(cmd);
            break;
        case "activate_composer":
            _executeActivateComposer(cmd);
            break;
        case "reveal_composer_in_new_agent":
            _executeRevealComposerInNewAgent(cmd);
            break;
        case "retry":
            _executeTask(cmd,true);
            break;
        case "stop":
            var preserveMcpWaiting=cmd.preserveMcpWaiting===true;
            try{
                var stopStates=[];
                for(var diagCid in _instances){if(_instances[diagCid])stopStates.push(_retryInstanceDiag(diagCid,_instances[diagCid]))}
                _rLog("diag stop command received taskId="+(cmd.taskId||"")+" preserveMcpWaiting="+(preserveMcpWaiting?1:0)+" instances=["+stopStates.join(" | ")+"]");
            }catch(e){_rLog("diag stop command state failed "+String(e&&e.message||e))}
            if(cmd.taskId){
                for(var cid in _instances){
                    if(_instances[cid]&&_instances[cid].taskId===cmd.taskId){
                        _rLog("diag stop task applying before "+_retryInstanceDiag(cid,_instances[cid]));
                        _instances[cid].running=false;
                        if(!preserveMcpWaiting)_releaseRuntimeChannel(_instances[cid].channelId,cid,"client_stop_task");
                        _rLog("received stop task "+cmd.taskId+" composer="+cid+" after "+_retryInstanceDiag(cid,_instances[cid]));
                    }
                }
            }else{
                _aborted=true;
                for(var cid in _instances){
                    if(_instances[cid]){
                        _rLog("diag stop all applying before "+_retryInstanceDiag(cid,_instances[cid]));
                        if(!preserveMcpWaiting)_releaseRuntimeChannel(_instances[cid].channelId,cid,"client_stop_all");
                        _instances[cid].running=false;
                        _rLog("diag stop all applied after "+_retryInstanceDiag(cid,_instances[cid]));
                    }
                }
                _rLog("received stop command");
                // 稍后恢复，允许新批次
                setTimeout(function(){_aborted=false},2000);
            }
            break;
        case "list_billing_composers":
            var popupIds=[];
            var roots=document.querySelectorAll(_SEL_COMPOSER_ROOT);
            for(var j=0;j<roots.length;j++){
                var r=roots[j];
                if(r.querySelector(_SEL_POPUP)){
                    var did=r.getAttribute("data-composer-id");
                    if(did)popupIds.push(did);
                }
            }
            _postReport({reqId:cmd.reqId,composerIds:popupIds});
            break;
        case "watch_session_set":
            _processWatchSet(cmd);
            break;
    }
}

// 启动轮询（260ms 间隔 + 3s XHR 超时）
_discoverBatchRetryPort(null,true);
_schedulePollCmd(260);
_rLog("Batch retry client loaded, port="+_batchRetryPort+" clientId="+qtClientId);
})();

})();
`;
}
// ─── 注入/恢复 workbench ───────────────────────────
function isInjected() {
    return getSeamlessInjectionStatus().current;
}
function isStartPromptAutomationInjected() {
    const wp = findWorkbenchJsPath();
    if (!wp || !fs.existsSync(wp)) {
        return false;
    }
    const content = fs.readFileSync(wp, 'utf-8');
    return content.includes(SEAMLESS_START_PROMPT_MARKER);
}
function isComposerBridgeInjected() {
    const wp = findWorkbenchJsPath();
    if (!wp || !fs.existsSync(wp)) {
        return false;
    }
    const content = fs.readFileSync(wp, 'utf-8');
    return content.includes(SEAMLESS_COMPOSER_BRIDGE_MARKER);
}
function isComposerServiceHooked() {
    const wp = findWorkbenchJsPath();
    if (!wp || !fs.existsSync(wp)) {
        return false;
    }
    const content = fs.readFileSync(wp, 'utf-8');
    return hasComposerServiceHook(content);
}
function pruneSeamlessRuntimeClients(now = Date.now()) {
    for (const [id, client] of Object.entries(seamlessRuntimeClients)) {
        if (now - client.lastSeen > SEAMLESS_RUNTIME_CLIENT_TTL_MS) {
            delete seamlessRuntimeClients[id];
        }
    }
}
function noteSeamlessRuntimeClient(clientId, focused, version, windowScopeId) {
    const id = clientId || 'unknown';
    seamlessRuntimeClients[id] = {
        clientId: id,
        lastSeen: Date.now(),
        focused,
        version,
        windowScopeId: String(windowScopeId || '').trim() || undefined
    };
}
// fallback 实例向 primary(36530) 探活，刷新 runtime 加载状态缓存。
// primary 实例自身有真实心跳数据，无需探活。
async function refreshPrimaryRuntimeProbe() {
    if (serverPort === SEAMLESS_PORT)
        return;
    const result = await postJsonToLocalPort(SEAMLESS_PORT, '/api/status', {}, 1500);
    if (result.ok && result.data) {
        primaryRuntimeProbe = {
            loaded: result.data.runtimeLoaded === true,
            anyLoaded: result.data.runtimeAnyLoaded === true || result.data.runtimeLoaded === true,
            count: Number(result.data.runtimeClientCount || 0) || 0,
            at: Date.now()
        };
    }
}
function getSeamlessRuntimeStatus() {
    const now = Date.now();
    pruneSeamlessRuntimeClients(now);
    const clients = Object.values(seamlessRuntimeClients)
        .sort((a, b) => b.lastSeen - a.lastSeen)
        .map(client => ({
        clientId: client.clientId,
        lastSeen: client.lastSeen,
        ageMs: now - client.lastSeen,
        focused: client.focused,
        version: client.version,
        windowScopeId: client.windowScopeId || ''
    }));
    let runtimeAnyLoaded = clients.length > 0;
    let runtimeLoaded = clients.some(client => client.version === SEAMLESS_RUNTIME_VERSION);
    let runtimeClientCount = clients.filter(client => client.version === SEAMLESS_RUNTIME_VERSION).length;
    // fallback 实例自身收不到心跳，合并最近一次 primary 探活结果，避免误判为“待重启”。
    if (serverPort !== SEAMLESS_PORT &&
        (now - primaryRuntimeProbe.at) < PRIMARY_RUNTIME_PROBE_TTL_MS) {
        runtimeLoaded = runtimeLoaded || primaryRuntimeProbe.loaded;
        runtimeAnyLoaded = runtimeAnyLoaded || primaryRuntimeProbe.anyLoaded;
        runtimeClientCount = Math.max(runtimeClientCount, primaryRuntimeProbe.count);
    }
    return {
        runtimeLoaded,
        runtimeAnyLoaded,
        runtimeCurrent: runtimeLoaded,
        expectedRuntimeVersion: SEAMLESS_RUNTIME_VERSION,
        runtimeClientCount,
        runtimeClients: clients
    };
}
function getBackupPath(wp) {
    return wp + '.qingtian.backup';
}
function stripQingTianInjection(content) {
    const markerIdx = content.indexOf(SEAMLESS_START_PROMPT_MARKER);
    if (markerIdx < 0) {
        return content;
    }
    const beforeMarker = content.lastIndexOf(SEAMLESS_MARKER, markerIdx);
    const idx = beforeMarker >= 0 && markerIdx - beforeMarker < 120 ? beforeMarker : markerIdx;
    if (idx < 0) {
        return content;
    }
    return content.slice(0, idx).trimEnd() + '\n';
}
function hasComposerServiceHook(content) {
    return content.includes(COMPOSER_SERVICE_HOOK_MARKER) || content.includes('window.__qtComposerService=this');
}
function findCreateComposerMethod(content) {
    const re = /(^|[{};,])(\s*)(async\s+)?createComposer\(([^)]*)\)\{/g;
    let best = null;
    let match;
    while ((match = re.exec(content)) !== null) {
        const start = match.index + match[1].length + match[2].length;
        const bodyStart = re.lastIndex;
        const after = content.slice(bodyStart, bodyStart + 2600);
        let score = 0;
        if (after.includes('createComposerImpl')) {
            score += 90;
        }
        if (after.includes('composerDataService')) {
            score += 45;
        }
        if (after.includes('composerChatService')) {
            score += 35;
        }
        if (after.includes('performance.now')) {
            score += 25;
        }
        if (after.includes('composer.createComposer')) {
            score += 10;
        }
        if (score > (best?.score ?? -1)) {
            best = { start, bodyStart, score };
        }
    }
    return best;
}
function patchComposerServiceHook(content) {
    let next = content;
    let methodHooked = false;
    let constructorHooked = false;
    const methodHook = `${COMPOSER_SERVICE_HOOK_MARKER}if(!window.__qtComposerService)window.__qtComposerService=this;`;
    const constructorHook = `${COMPOSER_SERVICE_HOOK_MARKER}window.__qtComposerService=this,`;
    const method = findCreateComposerMethod(next);
    if (method) {
        const constructorIndex = next.lastIndexOf('constructor(', method.start);
        if (constructorIndex >= 0 && method.start - constructorIndex < 120000) {
            const ctorSlice = next.slice(constructorIndex, method.start);
            if (!ctorSlice.includes('__qtComposerService')) {
                const superIndex = next.indexOf('super(),', constructorIndex);
                if (superIndex >= 0 && superIndex < method.start) {
                    const insertAt = superIndex + 'super(),'.length;
                    next = next.slice(0, insertAt) + constructorHook + next.slice(insertAt);
                    constructorHooked = true;
                }
            }
        }
    }
    const updatedMethod = findCreateComposerMethod(next);
    if (updatedMethod && !next.slice(updatedMethod.bodyStart, updatedMethod.bodyStart + 220).includes('__qtComposerService')) {
        next = next.slice(0, updatedMethod.bodyStart) + methodHook + next.slice(updatedMethod.bodyStart);
        methodHooked = true;
    }
    return {
        content: next,
        ok: hasComposerServiceHook(next),
        methodHooked,
        constructorHooked
    };
}
function validateJavaScriptSyntax(content) {
    try {
        // Parse only. Do not execute the generated Cursor workbench bundle.
        // eslint-disable-next-line no-new-func
        new Function(content);
        return { ok: true };
    }
    catch (err) {
        return { ok: false, error: String((err && err.stack) || (err && err.message) || err) };
    }
}
function isLikelyModuleSyntaxValidationError(error) {
    const text = String(error || '');
    return /Unexpected token ['"]?export['"]?/i.test(text) ||
        /Unexpected token ['"]?import['"]?/i.test(text) ||
        /Cannot use import statement outside a module/i.test(text);
}
function readFileHead(filePath, maxBytes = 800) {
    const fd = fs.openSync(filePath, 'r');
    try {
        const buf = Buffer.alloc(maxBytes);
        const n = fs.readSync(fd, buf, 0, maxBytes, 0);
        return buf.slice(0, n).toString('utf-8');
    }
    finally {
        fs.closeSync(fd);
    }
}
function isWorkbenchBackupCompatible(currentPath, backupPath) {
    try {
        if (!fs.existsSync(currentPath) || !fs.existsSync(backupPath)) {
            return false;
        }
        const curStat = fs.statSync(currentPath);
        const bakStat = fs.statSync(backupPath);
        // 体积差过大，几乎一定是跨 Cursor 大版本的旧备份
        if (bakStat.size > 0 && curStat.size > 0) {
            const ratio = bakStat.size / curStat.size;
            if (ratio < 0.75 || ratio > 1.35) {
                return false;
            }
        }
        // 备份比当前 workbench 更旧，且体积不一致 → 视为过期
        if (bakStat.mtimeMs + 1000 < curStat.mtimeMs && bakStat.size !== curStat.size) {
            return false;
        }
        const curHead = readFileHead(currentPath, 800);
        const bakHead = readFileHead(backupPath, 800);
        if (bakHead.includes('__QINGTIAN_SEAMLESS__')) {
            return false;
        }
        // 头部版权/构建戳不一致时拒绝使用备份
        if (curHead.slice(0, 120) !== bakHead.slice(0, 120)) {
            return false;
        }
        return true;
    }
    catch {
        return false;
    }
}
function invalidateStaleWorkbenchBackup(wp) {
    const bp = getBackupPath(wp);
    if (!fs.existsSync(bp)) {
        return;
    }
    if (isWorkbenchBackupCompatible(wp, bp)) {
        return;
    }
    try {
        const stalePath = `${bp}.stale.${Date.now()}`;
        fs.renameSync(bp, stalePath);
        console.warn('[QingTian] 已隔离过期 workbench 备份，避免黑屏:', stalePath);
    }
    catch (err) {
        try {
            fs.unlinkSync(bp);
            console.warn('[QingTian] 已删除过期 workbench 备份，避免黑屏');
        }
        catch (e2) {
            console.warn('[QingTian] 无法处理过期备份:', err, e2);
        }
    }
}
function patchAuthServiceHook(content) {
    // Cursor 新版本参数名会变（e/n/t...），不能写死 addLoginChangedListener(e)
    const exact = 'addLoginChangedListener(e){this.loginChangedListeners.push(e)}';
    if (content.includes(exact) && !content.includes('window.__qtAuthService=this')) {
        return content.replace(exact, 'addLoginChangedListener(e){this.loginChangedListeners.push(e);window.__qtAuthService=this}');
    }
    const flex = /addLoginChangedListener\(([A-Za-z_$][\w$]*)\)\{this\.loginChangedListeners\.push\(\1\)\}/;
    if (flex.test(content) && !content.includes('window.__qtAuthService=this')) {
        return content.replace(flex, 'addLoginChangedListener($1){this.loginChangedListeners.push($1);window.__qtAuthService=this}');
    }
    return content;
}
async function injectWorkbench() {
    const wp = findWorkbenchJsPath();
    if (!wp) {
        return { ok: false, message: '未找到 Cursor 的 workbench.desktop.main.js，请确认 Cursor 已安装' };
    }
    // 关键：永远以「当前安装目录里的 workbench」为底座。
    // 旧逻辑会优先读 .qingtian.backup；Cursor 升级后旧备份覆盖新文件 → 黑屏。
    invalidateStaleWorkbenchBackup(wp);
    const bp = getBackupPath(wp);
    let content = fs.readFileSync(wp, 'utf-8');
    const alreadyInjected = content.includes('__QINGTIAN_SEAMLESS__') || content.includes('__QINGTIAN_START_PROMPT_AUTOMATION_V7__');
    if (!alreadyInjected) {
        try {
            if (!fs.existsSync(bp) || !isWorkbenchBackupCompatible(wp, bp)) {
                fs.copyFileSync(wp, bp);
            }
        }
        catch (err) {
            console.warn('[QingTian] 备份 workbench 失败（继续注入）:', err);
        }
    }
    content = stripQingTianInjection(content);
    // 步骤1: 绕过完整性检查
    const t1 = '_showNotification(){';
    if (content.includes(t1) && !content.includes(`_showNotification(){${SEAMLESS_MARKER}`)) {
        content = content.replace(t1, `_showNotification(){${SEAMLESS_MARKER}return;`);
    }
    // 步骤2: hook AuthService — 暴露到 window（兼容新版压缩参数名）
    content = patchAuthServiceHook(content);
    // 步骤3: hook ComposerService — 暴露到 window.__qtComposerService
    const composerPatch = patchComposerServiceHook(content);
    content = composerPatch.content;
    if (!composerPatch.ok) {
        return {
            ok: false,
            message: 'ComposerService hook 未命中：当前 Cursor workbench 结构和插件预期不一致，批量会话重试桥接无法启用。为避免黑屏，未写入任何修改。'
        };
    }
    // 步骤4: 追加轮询脚本
    const injectionScript = buildInjectionScript(serverPort);
    const injectionSyntax = validateJavaScriptSyntax(injectionScript);
    if (!injectionSyntax.ok) {
        return {
            ok: false,
            message: '注入脚本语法校验失败，已阻止写入，避免 Cursor 重启白屏：' + (injectionSyntax.error || 'unknown')
        };
    }
    content += injectionScript;
    const finalSyntax = validateJavaScriptSyntax(content);
    if (!finalSyntax.ok && !isLikelyModuleSyntaxValidationError(finalSyntax.error)) {
        return {
            ok: false,
            message: 'Cursor workbench 注入后语法校验失败，已阻止写入，避免 Cursor 重启白屏：' + (finalSyntax.error || 'unknown')
        };
    }
    try {
        fs.writeFileSync(wp, content, 'utf-8');
    }
    catch (err) {
        const code = err && err.code;
        const msg = String((err && err.message) || err);
        if (code === 'EACCES' || code === 'EPERM' || /EACCES|EPERM|permission/i.test(msg)) {
            if (process.platform === 'darwin') {
                return {
                    ok: false,
                    message: '没有权限写入 workbench.js。Cursor 装在 /Applications 需要管理员权限。\n' +
                        '解决方法（任选一种）：\n' +
                        '  1) 终端运行：\n' +
                        '     sudo chown -R $USER /Applications/Cursor.app/Contents/Resources/app/out/vs/workbench/\n' +
                        '  2) 用「sudo open -a Cursor」启动一次 Cursor\n' +
                        '完成后重新点击注入。\n详细错误：' + msg
                };
            }
            if (process.platform === 'win32') {
                return {
                    ok: false,
                    message: '写入 workbench.js 被拒绝（' + code + '）。常见原因：\n' +
                        '  - Cursor 进程还在运行：请全部关闭后重试\n' +
                        '  - 杀毒/安全软件拦截：临时关闭后重试\n' +
                        '  - 仅限只读：右键 Cursor 安装目录 → 属性 → 取消“只读”\n' +
                        '详细错误：' + msg
                };
            }
            return {
                ok: false,
                message: 'Linux 写入被拒绝（' + code + '）。如果 Cursor 装在 /opt 等系统路径，\n' +
                    '请运行：sudo chown -R $USER /opt/Cursor/resources/app/out/vs/workbench/\n' +
                    '详细错误：' + msg
            };
        }
        return { ok: false, message: '写入 workbench.js 失败：' + msg };
    }
    return {
        ok: true,
        message: '注入成功！请重启 Cursor 生效（之后切换账号将无感完成）'
    };
}
async function restoreWorkbench() {
    const wp = findWorkbenchJsPath();
    if (!wp) {
        return { ok: false, message: '未找到 workbench.js' };
    }
    const bp = getBackupPath(wp);
    if (!fs.existsSync(bp)) {
        return { ok: false, message: '无备份文件，无法恢复' };
    }
    if (!isWorkbenchBackupCompatible(wp, bp)) {
        invalidateStaleWorkbenchBackup(wp);
        // 兼容性失败时：仅剥离注入，绝不回灌跨版本旧备份
        try {
            const current = fs.readFileSync(wp, 'utf-8');
            if (current.includes('__QINGTIAN_SEAMLESS__') || current.includes('__QINGTIAN_START_PROMPT_AUTOMATION_V7__')) {
                fs.writeFileSync(wp, stripQingTianInjection(current), 'utf-8');
                return { ok: true, message: '检测到过期备份，已仅移除注入标记（未回灌旧版 workbench）。请重启 Cursor' };
            }
        }
        catch { }
        return { ok: false, message: '备份与当前 Cursor 版本不兼容，已拒绝恢复，避免黑屏。请重装 Cursor 或从官方安装包恢复 workbench.desktop.main.js' };
    }
    fs.copyFileSync(bp, wp);
    return { ok: true, message: '已恢复原始文件，请重启 Cursor' };
}
// ─── HTTP 服务器 ──────────────────────────────────
function startServer() {
    return new Promise((resolve, reject) => {
        if (httpServer) {
            resolve(serverPort);
            return;
        }
        const server = http.createServer((req, res) => {
            // CORS
            res.setHeader('Access-Control-Allow-Origin', '*');
            res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
            res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
            res.setHeader('Content-Type', 'application/json; charset=utf-8');
            if (req.method === 'OPTIONS') {
                res.writeHead(200);
                res.end('{}');
                return;
            }
            const requestUrl = new URL(req.url || '/', 'http://127.0.0.1');
            const url = requestUrl.pathname;
            const clientId = String(requestUrl.searchParams.get('clientId') || '').trim();
            const windowScopeId = String(requestUrl.searchParams.get('windowScopeId') || '').trim();
            const clientFocused = requestUrl.searchParams.get('focused') === '1';
            if (url === '/api/health') {
                res.writeHead(200);
                res.end(JSON.stringify({ status: 'ok', port: serverPort }));
            }
            else if (url === '/api/runtime-heartbeat') {
                const version = String(requestUrl.searchParams.get('version') || '').trim();
                noteSeamlessRuntimeClient(clientId, clientFocused, version, windowScopeId);
                const bridgeMeta = getBatchBridgeMetaForClient(clientId);
                res.writeHead(200);
                res.end(JSON.stringify({ ok: true, port: serverPort, batchPort: bridgeMeta.port, batchWorkspaceScopeId: bridgeMeta.workspaceScopeId, ...getSeamlessRuntimeStatus() }));
            }
            else if (url === '/api/status') {
                const runtimeStatus = getSeamlessRuntimeStatus();
                const injectionStatus = getSeamlessInjectionStatus();
                res.writeHead(200);
                res.end(JSON.stringify({
                    injected: injectionStatus.current,
                    injectionInstalled: injectionStatus.injected,
                    injectionCurrent: injectionStatus.current,
                    injectedRuntimeVersion: injectionStatus.version,
                    expectedRuntimeVersion: injectionStatus.expectedVersion,
                    runtimeLoaded: runtimeStatus.runtimeLoaded,
                    runtimeAnyLoaded: runtimeStatus.runtimeAnyLoaded,
                    runtimeCurrent: runtimeStatus.runtimeCurrent,
                    runtimeClientCount: runtimeStatus.runtimeClientCount,
                    runtimeClients: runtimeStatus.runtimeClients,
                    startPromptAutomation: isStartPromptAutomationInjected(),
                    composerBridge: isComposerBridgeInjected(),
                    composerServiceHooked: isComposerServiceHooked(),
                    hasPending: pendingSwitch !== null,
                    pendingId: pendingSwitch?.id || null,
                    hasPendingStartPrompt: pendingStartPrompt !== null,
                    pendingStartPromptId: pendingStartPrompt?.id || null,
                    pendingStartPromptClaimedBy: pendingStartPrompt?.claimedBy || null,
                    pendingStartPromptClaimedAt: pendingStartPrompt?.claimedAt || null,
                    pendingEmail: pendingSwitch?.email || null,
                    port: serverPort
                }));
            }
            else if (url === '/api/batch-bridge-for') {
                // 注入脚本查询：我这个窗口(clientId)被哪个工作区引擎认领了
                res.writeHead(200);
                const bridgeMeta = getBatchBridgeMetaForClient(clientId);
                res.end(JSON.stringify({ ok: true, port: bridgeMeta.port, workspaceScopeId: bridgeMeta.workspaceScopeId }));
            }
            else if (url === '/api/register-batch-engine' && req.method === 'POST') {
                let body = '';
                req.on('data', chunk => { body += chunk; });
                req.on('end', () => {
                    let data = {};
                    try {
                        data = body ? JSON.parse(body) : {};
                    }
                    catch { }
                    registerBatchEngineLocal(Number(data.port) || 0, String(data.workspacePath || ''));
                    res.writeHead(200);
                    res.end(JSON.stringify({ ok: true }));
                });
            }
            else if (url === '/api/adopt-focused-window' && req.method === 'POST') {
                let body = '';
                req.on('data', chunk => { body += chunk; });
                req.on('end', () => {
                    let data = {};
                    try {
                        data = body ? JSON.parse(body) : {};
                    }
                    catch { }
                    const result = adoptFocusedWindowLocal(Number(data.port) || 0, String(data.workspacePath || ''));
                    res.writeHead(200);
                    res.end(JSON.stringify({ ok: result.ok, clientId: result.clientId || '' }));
                });
            }
            else if (url === '/api/pending-switch') {
                if (pendingSwitch) {
                    const now = Date.now();
                    if (now - pendingSwitch.createdAt > 60000) {
                        console.warn(`[QingTian] 待切换已过期: ${pendingSwitch.email}`);
                        pendingSwitch = null;
                        res.writeHead(200);
                        res.end('null');
                        return;
                    }
                    if (pendingSwitch.claimedAt && now - pendingSwitch.claimedAt < 4000 && pendingSwitch.claimedBy && pendingSwitch.claimedBy !== clientId) {
                        res.writeHead(200);
                        res.end('null');
                        return;
                    }
                    pendingSwitch.claimedBy = clientId || pendingSwitch.claimedBy;
                    pendingSwitch.claimedAt = now;
                    const data = pendingSwitch;
                    res.writeHead(200);
                    res.end(JSON.stringify(data));
                    console.log(`[QingTian] 注入脚本已领取待切换: ${data.email}, id=${data.id}, client=${clientId || ''}`);
                }
                else {
                    res.writeHead(200);
                    res.end('null');
                }
            }
            else if (url === '/api/switch-done' && req.method === 'POST') {
                let body = '';
                req.on('data', chunk => { body += chunk; });
                req.on('end', () => {
                    let data = {};
                    try {
                        data = body ? JSON.parse(body) : {};
                    }
                    catch { }
                    const id = String(data.id || pendingSwitch?.id || pendingSwitchWaiter?.id || '').trim();
                    const ok = data.ok !== false;
                    const error = String(data.error || '').trim();
                    const ackClientId = String(data.clientId || clientId || pendingSwitch?.claimedBy || '').trim();
                    const email = pendingSwitch?.email || pendingSwitchWaiter?.email || '';
                    const elapsedMs = pendingSwitchWaiter
                        ? Date.now() - pendingSwitchWaiter.startedAt
                        : pendingSwitch
                            ? Date.now() - pendingSwitch.createdAt
                            : 0;
                    const result = { ok, id, email, clientId: ackClientId, error, elapsedMs };
                    if (pendingSwitch && (!id || pendingSwitch.id === id)) {
                        pendingSwitch = null;
                    }
                    resolvePendingSwitchWaiter(result);
                    res.writeHead(200);
                    res.end(JSON.stringify({ ok: true }));
                    console.log(`[QingTian] 无感切号回执: ok=${ok}, id=${id}, client=${ackClientId}, error=${error}`);
                });
            }
            else if (url === '/api/enqueue-start-prompt' && req.method === 'POST') {
                let body = '';
                req.on('data', chunk => { body += chunk; });
                req.on('end', () => {
                    try {
                        const data = body ? JSON.parse(body) : {};
                        const prompt = String(data.prompt || '').trim();
                        if (!prompt) {
                            res.writeHead(400);
                            res.end(JSON.stringify({ ok: false, error: 'empty_prompt' }));
                            return;
                        }
                        const task = installPendingStartPrompt({
                            id: String(data.id || ''),
                            prompt,
                            channelId: String(data.channelId || '1'),
                            targetMode: data.targetMode === 'bound' ? 'bound' : 'active',
                            composerId: String(data.composerId || ''),
                            skippedClientIds: Array.isArray(data.skippedClientIds) ? data.skippedClientIds : [],
                            createdAt: Number(data.createdAt || Date.now()),
                            preferFocused: data.preferFocused !== false,
                            focusClaimUntil: Number(data.focusClaimUntil || (Date.now() + 5500)),
                            sourcePort: Number(data.sourcePort || serverPort),
                            source: String(data.source || 'enqueue')
                        });
                        res.writeHead(200);
                        res.end(JSON.stringify({ ok: true, id: task.id, port: serverPort }));
                    }
                    catch (e) {
                        res.writeHead(400);
                        res.end(JSON.stringify({ ok: false, error: e.message }));
                    }
                });
            }
            else if (url === '/api/pending-start-prompt') {
                if (pendingStartPrompt && Date.now() - pendingStartPrompt.createdAt < 120000) {
                    const now = Date.now();
                    if (clientId && pendingStartPrompt.skippedClientIds?.includes(clientId)) {
                        res.writeHead(200);
                        res.end('null');
                        return;
                    }
                    if (pendingStartPrompt.preferFocused !== false &&
                        pendingStartPrompt.targetMode !== 'bound' &&
                        pendingStartPrompt.focusClaimUntil &&
                        now < pendingStartPrompt.focusClaimUntil &&
                        !clientFocused) {
                        res.writeHead(200);
                        res.end('null');
                        console.log(`[QingTian] start prompt wait focused client: id=${pendingStartPrompt.id}, skipClient=${clientId || ''}`);
                        return;
                    }
                    if (pendingStartPrompt.claimedAt && now - pendingStartPrompt.claimedAt < 8000 && pendingStartPrompt.claimedBy && pendingStartPrompt.claimedBy !== clientId) {
                        res.writeHead(200);
                        res.end('null');
                    }
                    else {
                        pendingStartPrompt.claimedAt = now;
                        pendingStartPrompt.claimedBy = clientId || pendingStartPrompt.claimedBy;
                        res.writeHead(200);
                        res.end(JSON.stringify(pendingStartPrompt));
                        console.log(`[QingTian] start prompt claimed: id=${pendingStartPrompt.id}, CH-${pendingStartPrompt.channelId}, ` +
                            `client=${clientId || ''}, focused=${clientFocused}, target=${pendingStartPrompt.targetMode || 'active'}`);
                    }
                }
                else {
                    if (pendingStartPrompt) {
                        console.warn(`[QingTian] start prompt expired: id=${pendingStartPrompt.id}, CH-${pendingStartPrompt.channelId}`);
                    }
                    pendingStartPrompt = null;
                    res.writeHead(200);
                    res.end('null');
                }
            }
            else if (url === '/api/start-prompt-done' && req.method === 'POST') {
                let body = '';
                req.on('data', chunk => { body += chunk; });
                req.on('end', () => {
                    try {
                        const data = body ? JSON.parse(body) : {};
                        if (pendingStartPrompt && data.id === pendingStartPrompt.id) {
                            if (data.ok) {
                                const composerId = String(data.composerId || '').trim();
                                console.log(`[QingTian] start prompt done: id=${pendingStartPrompt.id}, CH-${pendingStartPrompt.channelId}, ` +
                                    `client=${String(data.clientId || '').trim()}, focused=${!!data.focused}, method=${String(data.method || '')}, composer=${composerId}`);
                                if (data.needsNativeEnter) {
                                    triggerNativeEnter().catch(err => {
                                        console.warn('[QingTian] native enter fallback failed:', err);
                                    });
                                }
                                pendingStartPrompt = null;
                            }
                            else {
                                pendingStartPrompt.lastError = String(data.error || 'unknown');
                                if (data.skip) {
                                    const skipped = String(data.clientId || '').trim();
                                    if (skipped) {
                                        pendingStartPrompt.skippedClientIds = Array.from(new Set([...(pendingStartPrompt.skippedClientIds || []), skipped]));
                                    }
                                    pendingStartPrompt.claimedAt = undefined;
                                    pendingStartPrompt.claimedBy = undefined;
                                    console.warn(`[QingTian] start prompt skipped: id=${pendingStartPrompt.id}, CH-${pendingStartPrompt.channelId}, ` +
                                        `client=${skipped}, error=${pendingStartPrompt.lastError}`);
                                }
                                else {
                                    pendingStartPrompt.claimedAt = undefined;
                                    pendingStartPrompt.claimedBy = undefined;
                                    console.warn(`[QingTian] start prompt retry: id=${pendingStartPrompt.id}, CH-${pendingStartPrompt.channelId}, ` +
                                        `client=${String(data.clientId || '').trim()}, error=${pendingStartPrompt.lastError}`);
                                }
                            }
                        }
                        res.writeHead(200);
                        res.end(JSON.stringify({ ok: true }));
                    }
                    catch (e) {
                        res.writeHead(400);
                        res.end(JSON.stringify({ ok: false, error: e.message }));
                    }
                });
            }
            else {
                res.writeHead(404);
                res.end('{"error":"not found"}');
            }
        });
        // 尝试监听端口
        server.listen(SEAMLESS_PORT, '127.0.0.1', () => {
            httpServer = server;
            serverPort = SEAMLESS_PORT;
            console.log(`[QingTian] 无感切号 HTTP 服务器已启动 :${serverPort}`);
            resolve(serverPort);
        });
        server.on('error', (err) => {
            if (err.code === 'EADDRINUSE') {
                // 端口被占用，尝试随机端口
                server.listen(0, '127.0.0.1', () => {
                    httpServer = server;
                    serverPort = server.address().port;
                    console.log(`[QingTian] 无感切号 HTTP 服务器已启动 :${serverPort} (备用端口)`);
                    // 本实例为 fallback：立即向 primary 探活一次，避免面板首开时注入状态误报“待重启”
                    void refreshPrimaryRuntimeProbe().catch(() => { });
                    resolve(serverPort);
                });
            }
            else {
                reject(err);
            }
        });
    });
}
function stopServer() {
    if (httpServer) {
        httpServer.close();
        httpServer = null;
        console.log('[QingTian] 无感切号 HTTP 服务器已停止');
    }
}
// ─── 设置待切换 ───────────────────────────────────
function resolvePendingSwitchWaiter(result) {
    if (!pendingSwitchWaiter) {
        return;
    }
    if (pendingSwitchWaiter.id && result.id && pendingSwitchWaiter.id !== result.id) {
        return;
    }
    const waiter = pendingSwitchWaiter;
    pendingSwitchWaiter = null;
    clearTimeout(waiter.timeout);
    waiter.resolve({
        ...result,
        id: result.id || waiter.id,
        email: result.email || waiter.email,
        elapsedMs: result.elapsedMs || (Date.now() - waiter.startedAt)
    });
}
function setPendingSwitch(token, email, refreshToken, machineIds, timeoutMs = 20000) {
    if (pendingSwitchWaiter) {
        const replaced = pendingSwitchWaiter;
        pendingSwitchWaiter = null;
        clearTimeout(replaced.timeout);
        replaced.resolve({
            ok: false,
            id: replaced.id,
            email: replaced.email,
            error: 'switch_replaced_by_new_request',
            elapsedMs: Date.now() - replaced.startedAt
        });
    }
    const id = Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
    const createdAt = Date.now();
    pendingSwitch = { id, token, email, refreshToken, machineIds, createdAt };
    console.log(`[QingTian] 待切换: ${email}, id=${id}`);
    return new Promise(resolve => {
        const timeout = setTimeout(() => {
            const claimed = pendingSwitch?.id === id ? (pendingSwitch.claimedBy || '') : '';
            if (pendingSwitch?.id === id) {
                pendingSwitch = null;
            }
            if (pendingSwitchWaiter?.id === id) {
                pendingSwitchWaiter = null;
            }
            resolve({
                ok: false,
                id,
                email,
                clientId: claimed,
                error: claimed ? `switch_ack_timeout_after_claim:${claimed}` : 'switch_ack_timeout_no_client',
                elapsedMs: Date.now() - createdAt
            });
        }, timeoutMs);
        pendingSwitchWaiter = { id, email, startedAt: createdAt, timeout, resolve };
    });
}
function runDetached(command, args) {
    return new Promise((resolve, reject) => {
        const child = (0, child_process_1.spawn)(command, args, {
            detached: true,
            stdio: 'ignore',
            windowsHide: true
        });
        child.once('error', reject);
        child.once('spawn', () => {
            child.unref();
            resolve();
        });
    });
}
// 同步等待结果版（用于需要拿 exit code / stderr 的场景，如 osascript）
function execFileAsync(file, args, timeoutMs = 5000) {
    return new Promise((resolve, reject) => {
        const cp = require('child_process');
        cp.execFile(file, args, { timeout: timeoutMs }, (err, stdout, stderr) => {
            if (err) {
                err.stderr = stderr;
                err.stdout = stdout;
                reject(err);
            }
            else {
                resolve({ stdout: String(stdout || ''), stderr: String(stderr || '') });
            }
        });
    });
}
function postJsonToLocalPort(port, endpoint, payload, timeoutMs = 1800) {
    return new Promise(resolve => {
        const body = JSON.stringify(payload || {});
        const req = http.request({
            hostname: '127.0.0.1',
            port,
            path: endpoint,
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Content-Length': Buffer.byteLength(body)
            }
        }, res => {
            let raw = '';
            res.on('data', chunk => { raw += chunk; });
            res.on('end', () => {
                let data = null;
                try {
                    data = raw ? JSON.parse(raw) : null;
                }
                catch { }
                const ok = res.statusCode !== undefined && res.statusCode >= 200 && res.statusCode < 300 && data?.ok !== false;
                resolve({ ok, data, error: ok ? undefined : (data?.error || `HTTP ${res.statusCode || 0}`) });
            });
        });
        req.on('error', err => resolve({ ok: false, error: err.message }));
        req.setTimeout(timeoutMs, () => {
            req.destroy();
            resolve({ ok: false, error: `timeout_${timeoutMs}ms` });
        });
        req.write(body);
        req.end();
    });
}
function installPendingStartPrompt(task) {
    pendingStartPrompt = {
        ...task,
        id: String(task.id || (Date.now().toString(36) + Math.random().toString(36).slice(2, 8))),
        prompt: String(task.prompt || ''),
        channelId: String(task.channelId || '1'),
        targetMode: task.targetMode === 'bound' ? 'bound' : 'active',
        composerId: String(task.composerId || ''),
        workspaceScopeId: String(task.workspaceScopeId || ''),
        skippedClientIds: Array.isArray(task.skippedClientIds)
            ? task.skippedClientIds.map(v => String(v || '').trim()).filter(Boolean)
            : [],
        createdAt: Number(task.createdAt || Date.now()),
        claimedAt: undefined,
        claimedBy: undefined,
        lastError: '',
        preferFocused: task.preferFocused !== false,
        focusClaimUntil: Number(task.focusClaimUntil || (Date.now() + 5500)),
        sourcePort: Number(task.sourcePort || serverPort),
        source: String(task.source || 'local')
    };
    console.log(`[QingTian] start prompt queued: id=${pendingStartPrompt.id}, CH-${pendingStartPrompt.channelId}, ` +
        `target=${pendingStartPrompt.targetMode || 'active'}, preferFocused=${pendingStartPrompt.preferFocused !== false}, ` +
        `source=${pendingStartPrompt.source || ''}:${pendingStartPrompt.sourcePort || ''}`);
    return pendingStartPrompt;
}
async function forwardPendingStartPromptToPrimary(task) {
    const result = await postJsonToLocalPort(SEAMLESS_PORT, '/api/enqueue-start-prompt', task);
    if (result.ok) {
        console.log(`[QingTian] start prompt forwarded to primary ${SEAMLESS_PORT}: id=${task.id}, CH-${task.channelId}`);
        return { ok: true };
    }
    console.warn(`[QingTian] start prompt forward failed: id=${task.id}, CH-${task.channelId}, error=${result.error || ''}`);
    return { ok: false, error: result.error || 'forward_failed' };
}
// mac 上 Accessibility 引导只弹一次
let macAccessibilityWarned = false;
async function triggerNativeEnter() {
    await new Promise(resolve => setTimeout(resolve, 180));
    if (process.platform === 'win32') {
        await runDetached('powershell.exe', [
            '-NoProfile',
            '-STA',
            '-Command',
            "$ws=New-Object -ComObject WScript.Shell; Start-Sleep -Milliseconds 80; $ws.SendKeys('{ENTER}')"
        ]);
        return;
    }
    if (process.platform === 'darwin') {
        // 同步等，抓 stderr 检测 Accessibility 未授权
        try {
            await execFileAsync('osascript', [
                '-e',
                'tell application "System Events" to key code 36'
            ], 5000);
        }
        catch (err) {
            const stderr = String((err && err.stderr) || '');
            const message = String((err && err.message) || '');
            const combined = stderr + ' ' + message;
            // -1719 / -1743：未授予 Accessibility / not allowed assistive access
            const looksLikeAccessibility = /-1719|-1743|not allowed|assistive|System Events/i.test(combined);
            if (looksLikeAccessibility && !macAccessibilityWarned) {
                macAccessibilityWarned = true;
                void vscode.window.showWarningMessage('macOS 还未授权 Cursor 模拟键盘，自动 Enter 兜底失败。请在「系统设置 → 隐私与安全 → 辅助功能」中勾选 Cursor后重试。', '打开系统设置').then(choice => {
                    if (choice === '打开系统设置') {
                        runDetached('open', [
                            'x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility'
                        ]).catch(() => { });
                    }
                });
            }
            throw err;
        }
        return;
    }
    await runDetached('sh', [
        '-lc',
        'command -v xdotool >/dev/null 2>&1 && xdotool key Return || true'
    ]);
}
async function tryOpenCursorComposer() {
    const candidates = [
        'composer.newAgentChat',
        'composer.createNew',
        'composer.openComposer',
        'workbench.action.chat.open',
        'aichat.newchataction'
    ];
    for (const command of candidates) {
        try {
            await vscode.commands.executeCommand(command);
            return command;
        }
        catch {
            // Cursor command ids differ between versions.
        }
    }
    return '';
}
async function sendStartPromptToCursor(prompt, channelId, options) {
    const text = String(prompt || '').trim();
    if (!text) {
        return { ok: false, message: '开场白为空', copied: false };
    }
    // 一键开场依赖注入；未注入时不再复制剪贴板兜底、不自动注入，直接提示先启用注入。
    if (!isStartPromptAutomationInjected()) {
        return {
            ok: false,
            copied: false,
            message: '一键开场需要先启用账号接管注入，请点击右上角圆点完成注入后再使用。'
        };
    }
    await vscode.env.clipboard.writeText(text);
    const shouldOpenComposer = options?.openComposer !== false;
    const targetMode = options?.targetMode === 'bound' ? 'bound' : 'active';
    const binding = channelComposerBindings[String(channelId || '1')];
    const command = shouldOpenComposer ? await tryOpenCursorComposer() : '';
    await new Promise(resolve => setTimeout(resolve, command ? 2200 : 250));
    const task = {
        id: Date.now().toString(36) + Math.random().toString(36).slice(2, 8),
        prompt: text,
        channelId: String(channelId || '1'),
        targetMode,
        composerId: targetMode === 'bound' ? (binding?.composerId || '') : '',
        workspaceScopeId: workspaceScopeId(vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || ''),
        createdAt: Date.now(),
        preferFocused: targetMode !== 'bound',
        focusClaimUntil: Date.now() + 5500,
        sourcePort: serverPort,
        source: serverPort === SEAMLESS_PORT ? 'local-primary' : 'forward-from-fallback'
    };
    let queuedOnPort = serverPort;
    if (serverPort !== SEAMLESS_PORT) {
        const forwarded = await forwardPendingStartPromptToPrimary(task);
        if (!forwarded.ok) {
            installPendingStartPrompt({
                ...task,
                source: 'local-fallback-after-forward-failed'
            });
        }
        else {
            queuedOnPort = SEAMLESS_PORT;
        }
    }
    else {
        installPendingStartPrompt(task);
    }
    console.log(`[QingTian] start prompt requested: id=${task.id}, CH-${task.channelId}, ` +
        `serverPort=${serverPort}, queuedOn=${queuedOnPort}, command=${command || '-'}, target=${targetMode}`);
    return {
        ok: true,
        copied: true,
        command,
        message: command
            ? `已打开 Cursor 对话并投递 CH-${channelId} 开场白，正在自动发送。`
            : targetMode === 'bound'
                ? `已向 CH-${channelId} 历史绑定的 Cursor 对话投递开场白。`
                : shouldOpenComposer
                    ? `已投递 CH-${channelId} 开场白；请手动打开 Cursor Chat，脚本会继续尝试自动填充发送。`
                    : `已向当前 Cursor 对话投递 CH-${channelId} 开场白，脚本会尝试在当前窗口填充发送。`
    };
}
function hasPendingSwitch() {
    return pendingSwitch !== null;
}
function getSeamlessPort() {
    return serverPort;
}
//# sourceMappingURL=seamlessSwitch.js.map