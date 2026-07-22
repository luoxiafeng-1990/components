<template>
  <div class="dtm-tab" v-loading="loading">
    <!-- Header -->
    <div class="dtm-hero">
      <div class="hero-bg"></div>
      <div class="hero-content">
        <div class="hero-left">
          <div class="hero-chip">Device Tree</div>
          <div class="hero-ver">设备树模块化浏览器</div>
          <div class="hero-sub" v-if="data.active_dtoverlay">
            已加载 DTBO: <strong>{{ data.active_dtoverlay }}</strong>
          </div>
        </div>
        <div class="hero-right">
          <div class="hero-stat">
            <span class="hs-val">{{ data.modules.length }}</span>
            <span class="hs-lbl">模块</span>
          </div>
          <div class="hero-stat">
            <span class="hs-val">{{ data.total_nodes }}</span>
            <span class="hs-lbl">节点</span>
          </div>
          <div class="hero-stat">
            <span class="hs-val">{{ data.total_overrides }}</span>
            <span class="hs-lbl">Override</span>
          </div>
          <div class="hero-stat">
            <span class="hs-val">{{ data.dtbo_files.length }}</span>
            <span class="hs-lbl">DTBO</span>
          </div>
        </div>
        <el-button class="hero-refresh" :icon="Refresh" circle @click="fetchData" :loading="loading" />
      </div>
    </div>

    <!-- Search -->
    <div class="search-bar">
      <el-input v-model="searchText" placeholder="搜索模块、节点路径、参数名..." clearable prefix-icon="Search" size="default" />
    </div>

    <!-- Modules -->
    <div class="modules-list">
      <div v-for="mod in filteredModules" :key="mod.name" class="module-card"
        :class="{ expanded: expandedModules.has(mod.name) }">
        <div class="module-header" @click="toggleModule(mod.name)">
          <div class="module-left">
            <div class="module-dot" :style="{ background: colorMap[mod.color] || '#9ca3af' }"></div>
            <div class="module-name">{{ mod.name }}</div>
            <el-tag v-if="mod.nodes.length" size="small" effect="plain" round>{{ mod.nodes.length }} 节点</el-tag>
          </div>
          <div class="module-right">
            <el-tag v-if="mod.active_overrides > 0" size="small" type="success" effect="dark" round>
              {{ mod.active_overrides }}/{{ mod.overrides.length }} Override
            </el-tag>
            <el-tag v-else-if="mod.overrides.length > 0" size="small" type="info" effect="plain" round>
              {{ mod.overrides.length }} Override
            </el-tag>
            <el-tag v-if="mod.active_dtbos > 0" size="small" type="warning" effect="dark" round>
              {{ mod.active_dtbos }} DTBO 已加载
            </el-tag>
            <el-tag v-else-if="mod.dtbos.length > 0" size="small" type="info" effect="plain" round>
              {{ mod.dtbos.length }} DTBO
            </el-tag>
            <svg class="chevron" :class="{ open: expandedModules.has(mod.name) }" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="6 9 12 15 18 9"/></svg>
          </div>
        </div>

        <div v-if="expandedModules.has(mod.name)" class="module-body">
          <!-- Nodes with expandable properties -->
          <div v-if="mod.nodes.length" class="section">
            <div class="section-title">
              <svg class="sec-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v6m0 6v6m11-7h-6m-6 0H1"/></svg>
              节点列表 ({{ mod.nodes.length }})
            </div>
            <div class="node-grid">
              <div v-for="node in mod.nodes" :key="node.path" class="node-card">
                <div class="node-header" @click="toggleNode(node.path)">
                  <div class="node-left">
                    <svg class="node-chevron" :class="{ open: expandedNodes.has(node.path) }" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="9 6 15 12 9 18"/></svg>
                    <code class="node-path">{{ node.path }}</code>
                  </div>
                  <div class="node-meta">
                    <el-tag v-if="node.status" size="small" :type="node.status === 'okay' ? 'success' : 'danger'" effect="plain">{{ node.status }}</el-tag>
                    <span v-if="node.compatible" class="node-compat">{{ node.compatible }}</span>
                    <span class="prop-count">{{ node.properties?.length || 0 }} 属性</span>
                  </div>
                </div>
                <div v-if="expandedNodes.has(node.path) && node.properties?.length" class="node-props">
                  <table class="props-table">
                    <thead>
                      <tr><th>属性名</th><th>值</th><th>说明</th><th>大小</th></tr>
                    </thead>
                    <tbody>
                      <tr v-for="prop in node.properties" :key="prop.key" :class="{ 'prop-highlight': isOverrideTarget(mod, node.path, prop.key) }">
                        <td class="prop-key">
                          <code>{{ prop.key }}</code>
                          <el-tag v-if="isOverrideTarget(mod, node.path, prop.key)" size="small" type="warning" effect="plain" style="margin-left:4px">被 Override</el-tag>
                        </td>
                        <td class="prop-val"><code>{{ prop.value }}</code></td>
                        <td class="prop-desc">{{ propDesc(prop.key) }}</td>
                        <td class="prop-size">{{ prop.size }}B</td>
                      </tr>
                    </tbody>
                  </table>
                </div>
              </div>
            </div>
          </div>

          <!-- Overrides -->
          <div v-if="mod.overrides.length" class="section">
            <div class="section-title">
              <svg class="sec-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 5H2v7l6.29 6.29c.94.94 2.48.94 3.42 0l4.58-4.58c.94-.94.94-2.48 0-3.42L9 5z"/><circle cx="6" cy="9" r="1"/></svg>
              Override 映射 (config.txt → DTB)
              <el-tag v-if="mod.active_overrides > 0" size="small" type="success" effect="plain" style="margin-left:8px">{{ mod.active_overrides }} 个生效</el-tag>
            </div>
            <div class="override-cards">
              <div v-for="ovr in mod.overrides" :key="ovr.param" class="override-card" :class="{ active: ovr.active }">
                <div class="ovr-header">
                  <code class="ovr-param">{{ ovr.param }}</code>
                  <el-tag v-if="ovr.active" size="small" type="success" effect="dark" round>生效</el-tag>
                  <el-tag v-else size="small" type="info" effect="plain" round>未设</el-tag>
                </div>
                <div class="ovr-body">
                  <div class="ovr-row" v-if="ovr.target_path">
                    <span class="ovr-label">目标:</span>
                    <code class="ovr-target">{{ ovr.target_path }}</code>
                    <span class="ovr-sep">/</span>
                    <code class="ovr-prop">{{ ovr.target_prop }}</code>
                  </div>
                  <div class="ovr-row" v-if="ovr.active">
                    <span class="ovr-label">设定值:</span>
                    <code class="ovr-val">{{ formatValue(ovr.param, ovr.value) }}</code>
                  </div>
                </div>
              </div>
            </div>
          </div>

          <!-- DTBOs -->
          <div v-if="mod.dtbos.length" class="section">
            <div class="section-title">
              <svg class="sec-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 00-2 2v16a2 2 0 002 2h12a2 2 0 002-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
              DTBO 覆盖
            </div>
            <div class="dtbo-list">
              <div v-for="dtbo in mod.dtbos" :key="dtbo.file" class="dtbo-card" :class="{ active: dtbo.active }">
                <div class="dtbo-header" @click="toggleDtbo(dtbo.file)">
                  <div class="dtbo-left">
                    <el-tag :type="dtbo.active ? 'success' : 'info'" size="small" effect="plain" round>
                      {{ dtbo.active ? '已启用' : '可用' }}
                    </el-tag>
                    <code class="mono dtbo-name">{{ dtbo.file }}</code>
                  </div>
                  <div class="dtbo-right">
                    <el-button size="small" text type="primary" @click.stop="loadDtboDetail(dtbo.file)">
                      {{ expandedDtbos.has(dtbo.file) ? '收起' : '查看 DTS 内容' }}
                    </el-button>
                  </div>
                </div>
                <div v-if="expandedDtbos.has(dtbo.file)" class="dtbo-detail">
                  <div v-if="dtboCache[dtbo.file]" class="dtbo-content">
                    <div class="dtbo-meta">
                      <el-tag effect="plain" size="small" type="info">{{ fmtBytes(dtboCache[dtbo.file].size) }}</el-tag>
                      <span v-if="dtboCache[dtbo.file].fragments.length" class="frag-count">
                        {{ dtboCache[dtbo.file].fragments.length }} 个 Fragment
                      </span>
                    </div>
                    <div v-if="dtboCache[dtbo.file].fragments.length" class="frag-list">
                      <div v-for="(f, i) in dtboCache[dtbo.file].fragments" :key="i" class="frag-row">
                        <code class="mono">{{ f.fragment }}</code>
                        <span class="frag-target">→ {{ f.target_line }}</span>
                      </div>
                    </div>
                    <pre v-if="dtboCache[dtbo.file].dts_available" class="terminal">{{ dtboCache[dtbo.file].dts_content }}</pre>
                    <el-alert v-if="dtboCache[dtbo.file].dts_error" :title="dtboCache[dtbo.file].dts_error" type="error" :closable="false" />
                  </div>
                  <div v-else class="dtbo-loading">
                    <el-icon class="is-loading"><Loading /></el-icon> 加载中...
                  </div>
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, computed, onMounted } from 'vue'
import { Refresh, Loading } from '@element-plus/icons-vue'
import { systemApi, type DeviceTreeModulesData, type DeviceTreeModule, type DtboDetail } from '../../api'

const loading = ref(false)
const data = ref<DeviceTreeModulesData>({
  modules: [], total_nodes: 0, total_overrides: 0,
  active_dtoverlay: '', dtbo_files: []
})
const searchText = ref('')
const expandedModules = ref(new Set<string>())
const expandedNodes = ref(new Set<string>())
const expandedDtbos = ref(new Set<string>())
const dtboCache = reactive<Record<string, DtboDetail>>({})

const colorMap: Record<string, string> = {
  blue: '#3b82f6', purple: '#8b5cf6', green: '#10b981', cyan: '#06b6d4',
  red: '#ef4444', pink: '#ec4899', orange: '#f97316', yellow: '#eab308',
  teal: '#14b8a6', indigo: '#6366f1', brown: '#92400e', lime: '#84cc16',
  gray: '#6b7280', violet: '#7c3aed', steel: '#64748b', amber: '#f59e0b',
}

const filteredModules = computed(() => {
  if (!searchText.value) return data.value.modules
  const q = searchText.value.toLowerCase()
  return data.value.modules.filter(m => {
    if (m.name.toLowerCase().includes(q)) return true
    if (m.nodes.some(n => n.path.toLowerCase().includes(q) || n.compatible.toLowerCase().includes(q))) return true
    if (m.overrides.some(o => o.param.toLowerCase().includes(q))) return true
    if (m.dtbos.some(d => d.file.toLowerCase().includes(q))) return true
    return false
  })
})

function toggleModule(name: string) {
  if (expandedModules.value.has(name)) expandedModules.value.delete(name)
  else expandedModules.value.add(name)
}

function toggleNode(path: string) {
  if (expandedNodes.value.has(path)) expandedNodes.value.delete(path)
  else expandedNodes.value.add(path)
}

function toggleDtbo(file: string) {
  if (expandedDtbos.value.has(file)) expandedDtbos.value.delete(file)
  else expandedDtbos.value.add(file)
}

function isOverrideTarget(mod: DeviceTreeModule, nodePath: string, propKey: string): boolean {
  return mod.overrides.some(o =>
    o.active && o.target_path && nodePath.endsWith(o.target_path.split('/').pop() || '') && o.target_prop === propKey
  )
}

const propDescMap: Record<string, string> = {
  'compatible': '设备兼容性标识，驱动匹配依据',
  '#address-cells': '子节点地址字段占用的 cell 数（1 cell = 32位）',
  '#size-cells': '子节点大小字段占用的 cell 数',
  'reg': '设备寄存器地址和大小（或 CPU 编号）',
  'status': '设备状态：okay=已启用, disabled=已禁用',
  'phandle': '节点的唯一引用句柄（其他节点通过此值引用它）',
  'device_type': '设备类型标识（如 cpu、memory）',
  'model': '设备型号名称',
  'interrupt-parent': '中断控制器的 phandle 引用',
  'interrupts': '中断号和触发类型',
  'interrupt-controller': '标记此节点为中断控制器',
  '#interrupt-cells': '中断描述符占用的 cell 数',
  'clocks': '时钟源的 phandle 引用',
  'clock-names': '时钟源名称列表',
  'clock-frequency': '时钟频率（Hz）',
  'assigned-clocks': '指定需要设置频率的时钟',
  'assigned-clock-rates': '为 assigned-clocks 指定的目标频率',
  'resets': '复位控制器的 phandle 引用',
  'reset-names': '复位信号名称列表',
  'dma-ranges': 'DMA 地址映射范围',
  'ranges': '父子地址空间映射关系',
  'mmu-type': 'MMU 类型（如 riscv,sv39 = 39位虚拟地址）',
  'riscv,isa': 'RISC-V 指令集扩展字符串',
  'riscv,cbom-block-size': 'RISC-V CBO（缓存块操作）块大小',
  'cci-control-port': 'CCI 缓存一致性互联端口编号',
  'timebase-frequency': '系统定时器基准频率（Hz）',
  'max-frequency': '最大工作频率（Hz）',
  'min-frequency': '最小工作频率（Hz）',
  'bus-width': '总线宽度（位）',
  'no-1-8-v': '不支持 1.8V 信号电平',
  'broken-cd': '无物理卡检测引脚',
  'non-removable': '不可移除设备（如 eMMC）',
  'cap-mmc-highspeed': '支持 MMC 高速模式',
  'cap-sd-highspeed': '支持 SD 高速模式',
  'pinctrl-0': '默认引脚配置引用',
  'pinctrl-names': '引脚配置状态名列表',
  'gpio-controller': '标记此节点为 GPIO 控制器',
  '#gpio-cells': 'GPIO 描述符占用的 cell 数',
  'phy-mode': '网络 PHY 接口模式（如 rgmii）',
  'phy-handle': 'PHY 设备的 phandle 引用',
  'local-mac-address': '网络设备 MAC 地址',
  'tx-delay': '发送延迟（纳秒）',
  'rx-delay': '接收延迟（纳秒）',
  'memory-region': '关联的预留内存区域',
  'no-map': '此内存区域不映射到内核地址空间',
  'size': '预留内存大小（字节）',
  'alignment': '内存对齐要求（字节）',
  'alloc-ranges': '可分配的地址范围',
  'power-domains': '电源域的 phandle 引用',
  'operating-points-v2': '电压-频率对照表引用',
  'voltages': '电压配置值（微伏）',
}

function propDesc(key: string): string {
  if (propDescMap[key]) return propDescMap[key]
  if (key.startsWith('pinctrl-')) return '引脚配置引用'
  if (key.includes('clock')) return '时钟相关配置'
  if (key.includes('interrupt')) return '中断相关配置'
  if (key.includes('gpio')) return 'GPIO 相关配置'
  if (key.includes('reset')) return '复位相关配置'
  if (key.includes('dma')) return 'DMA 相关配置'
  if (key.includes('power')) return '电源管理相关'
  if (key.includes('freq')) return '频率配置'
  return ''
}

function formatValue(key: string, val: string | null) {
  if (!val) return ''
  if (key.includes('freq') && /^\d+$/.test(val)) {
    const n = parseInt(val)
    if (n >= 1e9) return `${(n / 1e9).toFixed(2)} GHz`
    if (n >= 1e6) return `${(n / 1e6).toFixed(0)} MHz`
    return val
  }
  return val
}

function fmtBytes(bytes?: number) {
  if (!bytes) return '--'
  if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${bytes} B`
}

async function loadDtboDetail(file: string) {
  if (expandedDtbos.value.has(file)) {
    expandedDtbos.value.delete(file)
    return
  }
  expandedDtbos.value.add(file)
  if (dtboCache[file]) return
  try {
    const res = await systemApi.dtboDetail('overlays/' + file)
    dtboCache[file] = res.data.data
  } catch { /* ignore */ }
}

async function fetchData() {
  loading.value = true
  try {
    const res = await systemApi.deviceTreeModules()
    data.value = res.data.data
  } catch { /* ignore */ }
  loading.value = false
}

onMounted(() => fetchData())
</script>

<style scoped>
.dtm-tab { display: flex; flex-direction: column; gap: 16px; }

/* Hero */
.dtm-hero {
  position: relative; border-radius: 16px; overflow: hidden;
  background: linear-gradient(135deg, #0c2340 0%, #1a365d 50%, #2a4a7f 100%);
}
.hero-bg {
  position: absolute; inset: 0;
  background:
    radial-gradient(ellipse 60% 50% at 80% 20%, rgba(59,130,246,0.15), transparent),
    radial-gradient(ellipse 40% 60% at 10% 80%, rgba(16,185,129,0.10), transparent);
  pointer-events: none;
}
.hero-content {
  position: relative; display: flex; align-items: center; gap: 24px;
  padding: 24px 28px; color: #fff; flex-wrap: wrap;
}
.hero-left { flex: 1; min-width: 200px; }
.hero-chip {
  display: inline-block; font-size: 11px; font-weight: 700; letter-spacing: 2px;
  text-transform: uppercase; padding: 3px 10px; border-radius: 20px;
  background: rgba(59,130,246,0.2); border: 1px solid rgba(59,130,246,0.35);
  color: #93c5fd; margin-bottom: 6px;
}
.hero-ver { font-size: 22px; font-weight: 800; letter-spacing: 0.3px; }
.hero-sub { font-size: 12px; color: rgba(255,255,255,0.55); margin-top: 4px; }
.hero-sub strong { color: #fbbf24; }
.hero-right { display: flex; gap: 12px; flex-wrap: wrap; }
.hero-stat {
  display: flex; flex-direction: column; align-items: center; min-width: 56px;
  background: rgba(255,255,255,0.06); border-radius: 10px; padding: 6px 12px;
  border: 1px solid rgba(255,255,255,0.08);
}
.hs-val { font-size: 18px; font-weight: 700; color: #e0e7ff; }
.hs-lbl { font-size: 9px; color: rgba(255,255,255,0.45); margin-top: 1px; text-transform: uppercase; letter-spacing: 0.5px; }
.hero-refresh { position: absolute; top: 14px; right: 14px; color: rgba(255,255,255,0.6) !important; }

/* Search */
.search-bar { max-width: 480px; }

/* Module cards */
.modules-list { display: flex; flex-direction: column; gap: 8px; }
.module-card {
  background: #fff; border-radius: 12px; border: 1px solid #f0f1f3;
  overflow: hidden; transition: border-color 0.2s;
}
.module-card.expanded { border-color: #dbeafe; }
.module-header {
  display: flex; justify-content: space-between; align-items: center;
  padding: 14px 18px; cursor: pointer; user-select: none;
  transition: background 0.15s;
}
.module-header:hover { background: #fafbfc; }
.module-left { display: flex; align-items: center; gap: 10px; }
.module-dot { width: 10px; height: 10px; border-radius: 50%; flex-shrink: 0; }
.module-name { font-size: 15px; font-weight: 700; color: #111827; }
.module-right { display: flex; align-items: center; gap: 6px; }
.chevron { width: 16px; height: 16px; color: #9ca3af; transition: transform 0.2s; flex-shrink: 0; margin-left: 4px; }
.chevron.open { transform: rotate(180deg); }

/* Module body */
.module-body { padding: 0 18px 16px; }
.section { margin-top: 16px; }
.section-title {
  display: flex; align-items: center; gap: 6px;
  font-size: 13px; font-weight: 600; color: #374151; margin-bottom: 8px;
}
.sec-icon { width: 15px; height: 15px; color: #6b7280; flex-shrink: 0; }

/* Node cards - expandable */
.node-grid { display: flex; flex-direction: column; gap: 4px; }
.node-card {
  border: 1px solid #e5e7eb; border-radius: 8px; overflow: hidden;
  background: #fafbfc;
}
.node-header {
  display: flex; justify-content: space-between; align-items: center;
  padding: 8px 12px; cursor: pointer; user-select: none;
  transition: background 0.15s;
}
.node-header:hover { background: #f3f4f6; }
.node-left { display: flex; align-items: center; gap: 6px; }
.node-chevron {
  width: 14px; height: 14px; color: #9ca3af; transition: transform 0.2s; flex-shrink: 0;
}
.node-chevron.open { transform: rotate(90deg); }
.node-path { font-family: 'JetBrains Mono', monospace; font-size: 12px; color: #1e40af; font-weight: 600; }
.node-meta { display: flex; align-items: center; gap: 6px; flex-shrink: 0; }
.node-compat { font-size: 11px; color: #6b7280; max-width: 200px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.prop-count { font-size: 11px; color: #9ca3af; }

/* Node properties table */
.node-props {
  border-top: 1px solid #e5e7eb; background: #fff; padding: 8px 12px;
}
.props-table {
  width: 100%; border-collapse: collapse; font-size: 12px;
}
.props-table th {
  text-align: left; padding: 4px 8px; font-weight: 600; color: #6b7280;
  border-bottom: 1px solid #e5e7eb; font-size: 11px; text-transform: uppercase;
  letter-spacing: 0.5px;
}
.props-table td {
  padding: 4px 8px; border-bottom: 1px solid #f3f4f6; vertical-align: top;
}
.prop-key code { font-family: 'JetBrains Mono', monospace; font-size: 11px; color: #374151; font-weight: 500; }
.prop-val code {
  font-family: 'JetBrains Mono', monospace; font-size: 11px; color: #059669;
  word-break: break-all;
}
.prop-desc { font-size: 11px; color: #6b7280; max-width: 250px; line-height: 1.4; }
.prop-size { font-size: 11px; color: #9ca3af; white-space: nowrap; }
.prop-highlight { background: #fffbeb !important; }
.prop-highlight td { border-bottom-color: #fde68a; }

/* Override cards */
.override-cards { display: flex; flex-direction: column; gap: 4px; }
.override-card {
  border: 1px solid #e5e7eb; border-radius: 8px; padding: 10px 14px;
  background: #fafbfc; transition: background 0.15s, border-color 0.15s;
}
.override-card.active { background: #f0fdf4; border-color: #bbf7d0; }
.ovr-header { display: flex; align-items: center; gap: 8px; margin-bottom: 4px; }
.ovr-param { font-family: 'JetBrains Mono', monospace; font-size: 13px; font-weight: 600; color: #111827; }
.ovr-body { padding-left: 4px; }
.ovr-row { display: flex; align-items: center; gap: 6px; font-size: 12px; margin-top: 2px; }
.ovr-label { color: #6b7280; font-size: 11px; min-width: 50px; }
.ovr-target { font-family: 'JetBrains Mono', monospace; font-size: 11px; color: #2563eb; }
.ovr-sep { color: #d1d5db; }
.ovr-prop { font-family: 'JetBrains Mono', monospace; font-size: 11px; color: #7c3aed; }
.ovr-val { font-family: 'JetBrains Mono', monospace; font-size: 12px; color: #059669; font-weight: 600; }

/* DTBO list - inline expandable */
.dtbo-list { display: flex; flex-direction: column; gap: 4px; }
.dtbo-card {
  border: 1px solid #e5e7eb; border-radius: 8px; overflow: hidden;
  background: #fafbfc; transition: border-color 0.15s;
}
.dtbo-card.active { border-color: #bbf7d0; background: #f0fdf4; }
.dtbo-header {
  display: flex; justify-content: space-between; align-items: center;
  padding: 8px 12px;
}
.dtbo-left { display: flex; align-items: center; gap: 8px; }
.dtbo-right { flex-shrink: 0; }
.mono { font-family: 'JetBrains Mono', monospace; }
.dtbo-name { font-size: 12px; color: #374151; font-weight: 500; }
.dtbo-detail { border-top: 1px solid #e5e7eb; padding: 10px 14px; background: #fff; }
.dtbo-meta { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
.frag-count { font-size: 12px; color: #6b7280; }
.frag-list { margin-bottom: 8px; }
.frag-row {
  display: flex; align-items: center; gap: 8px;
  padding: 3px 8px; background: #fffbeb; border-radius: 4px; border: 1px solid #fde68a;
  font-size: 12px; margin-bottom: 3px;
}
.frag-target { color: #92400e; font-size: 11px; }
.dtbo-loading { display: flex; align-items: center; gap: 8px; font-size: 13px; color: #6b7280; padding: 8px; }
.terminal {
  background: #111827; color: #d1d5db; padding: 12px 14px; border-radius: 8px;
  font-family: 'JetBrains Mono', monospace; font-size: 11px;
  line-height: 1.5; max-height: 400px; overflow: auto; white-space: pre; margin: 0;
}
</style>
