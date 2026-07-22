<template>
  <div class="board-tab" v-loading="loading">
    <!-- Hero: 板卡类型 -->
    <div class="board-hero">
      <div class="hero-bg"></div>
      <div class="hero-content">
        <div class="hero-left">
          <div class="hero-chip">Board Config</div>
          <div class="hero-ver">{{ boardType || '未知板型' }}</div>
          <div class="hero-sub" v-if="config.board_model">{{ config.board_model }}</div>
          <div class="hero-sub" v-if="config.board_compatible">{{ config.board_compatible }}</div>
        </div>
        <div class="hero-right">
          <div class="hero-stat" v-for="s in heroStats" :key="s.label">
            <span class="hs-val">{{ s.value }}</span>
            <span class="hs-lbl">{{ s.label }}</span>
          </div>
        </div>
        <el-button class="hero-refresh" :icon="Refresh" circle @click="fetchConfig" :loading="loading" />
      </div>
    </div>

    <el-alert v-if="!config.boot_dir_exists" title="/boot/firmware 目录不存在" type="warning"
      description="当前系统可能未挂载启动分区，板级配置信息不可用" show-icon :closable="false" style="margin-bottom:16px" />

    <!-- 活跃参数（分组显示） -->
    <section class="glass-card" v-if="Object.keys(config.active_params || {}).length">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 20V10"/><path d="M18 20V4"/><path d="M6 20v-4"/></svg>
          当前生效配置
        </div>
        <el-tag size="small" effect="dark" type="success" round>{{ Object.keys(config.active_params || {}).length }} 项</el-tag>
      </div>
      <div class="param-groups">
        <div v-for="(params, section) in groupedActiveParams" :key="section" class="param-group">
          <div class="param-group-title">{{ section }}</div>
          <div class="param-grid">
            <div v-for="(val, key) in params" :key="key" class="param-item">
              <span class="param-key">{{ key }}</span>
              <span class="param-val" :class="paramClass(key as string, val as string)">{{ formatValue(key as string, val as string) }}</span>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- 注释掉的参数（可选展开） -->
    <section class="glass-card" v-if="Object.keys(config.commented_params || {}).length">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15a2 2 0 01-2 2H7l-4 4V5a2 2 0 012-2h14a2 2 0 012 2z"/></svg>
          未启用配置（已注释）
        </div>
        <el-tag size="small" effect="plain" type="info" round>{{ Object.keys(config.commented_params || {}).length }} 项</el-tag>
      </div>
      <el-collapse>
        <el-collapse-item title="展开查看" name="commented">
          <div class="param-grid">
            <div v-for="(val, key) in config.commented_params" :key="key" class="param-item commented">
              <span class="param-key">{{ key }}</span>
              <span class="param-val muted">{{ val }}</span>
            </div>
          </div>
        </el-collapse-item>
      </el-collapse>
    </section>

    <!-- DTB/DTBO 文件 -->
    <section class="glass-card" v-if="dtbFiles.length">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 00-2 2v16a2 2 0 002 2h12a2 2 0 002-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
          设备树文件
        </div>
        <el-tag size="small" effect="plain" round>{{ dtbFiles.length }}</el-tag>
      </div>
      <div class="dtb-tags">
        <el-tag v-for="f in dtbFiles" :key="f" size="small"
          :type="f.endsWith('.dtbo') ? 'warning' : 'primary'" class="dtb-tag" effect="plain">
          {{ f }}
        </el-tag>
      </div>
    </section>

    <!-- Boot 分区文件列表 -->
    <section class="glass-card" v-if="bootFiles.length">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 01-2 2H4a2 2 0 01-2-2V5a2 2 0 012-2h5l2 3h9a2 2 0 012 2z"/></svg>
          /boot/firmware 文件列表
        </div>
        <el-tag size="small" effect="plain" round>{{ bootFiles.length }}</el-tag>
      </div>
      <el-table :data="bootFiles" size="small" stripe max-height="280" class="boot-table">
        <el-table-column prop="name" label="文件名" min-width="260" sortable show-overflow-tooltip />
        <el-table-column label="大小" width="100" sortable :sort-method="(a: any, b: any) => (a.size || 0) - (b.size || 0)">
          <template #default="{ row }">
            <span v-if="row.type === 'file'">{{ fmtBytes(row.size) }}</span>
            <span v-else class="muted">目录</span>
          </template>
        </el-table-column>
      </el-table>
    </section>

    <!-- Device Tree __overrides__ 映射表 -->
    <section class="glass-card" v-if="dtInfo && dtInfo.overrides.length">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 5H2v7l6.29 6.29c.94.94 2.48.94 3.42 0l4.58-4.58c.94-.94.94-2.48 0-3.42L9 5z"/><circle cx="6" cy="9" r="1"/></svg>
          __overrides__ 映射表
        </div>
        <el-tag size="small" effect="dark" type="warning" round>{{ dtInfo.overrides.length }} 项</el-tag>
      </div>
      <p class="gc-desc">config.txt 中的 dtparam 参数通过此映射表定位到 DTB 中的目标节点和属性</p>
      <el-table :data="dtInfo.overrides" size="small" stripe max-height="420" class="override-table"
        :default-sort="{ prop: 'name', order: 'ascending' }">
        <el-table-column prop="name" label="参数名" width="160" sortable show-overflow-tooltip>
          <template #default="{ row }">
            <code class="mono">{{ row.name }}</code>
            <el-tag v-if="isActiveOverride(row.name)" size="small" type="success" effect="plain" class="active-badge">生效中</el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="target_path" label="目标节点" min-width="200" sortable show-overflow-tooltip>
          <template #default="{ row }">
            <code class="mono node-path">{{ row.target_path || '(未解析)' }}</code>
          </template>
        </el-table-column>
        <el-table-column prop="target_prop" label="目标属性" width="140" sortable show-overflow-tooltip>
          <template #default="{ row }">
            <code class="mono prop-name">{{ row.target_prop || '--' }}</code>
          </template>
        </el-table-column>
        <el-table-column prop="phandle" label="phandle" width="90" sortable>
          <template #default="{ row }">
            <span class="mono hex-val">0x{{ (row.phandle || 0).toString(16) }}</span>
          </template>
        </el-table-column>
      </el-table>
    </section>

    <!-- 设备树节点概览 -->
    <section class="glass-card" v-if="dtInfo && dtInfo.top_nodes.length">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 12h-4l-3 9L9 3l-3 9H2"/></svg>
          设备树节点概览
        </div>
        <el-tag size="small" effect="plain" round>{{ dtInfo.top_nodes.length }} 个顶层节点</el-tag>
      </div>
      <el-collapse>
        <el-collapse-item v-for="node in dtInfo.top_nodes" :key="node.name" :name="node.name">
          <template #title>
            <div class="node-title">
              <code class="mono">{{ node.name }}</code>
              <el-tag v-if="node.status" size="small" :type="node.status === 'okay' ? 'success' : 'info'" effect="plain">{{ node.status }}</el-tag>
              <span v-if="node.compatible" class="node-compat">{{ node.compatible }}</span>
              <el-tag v-if="node.children && node.children.length" size="small" effect="plain" type="info" round>{{ node.children.length }} 子节点</el-tag>
            </div>
          </template>
          <div class="node-children" v-if="node.children && node.children.length">
            <div v-for="child in node.children" :key="child.name" class="child-item">
              <code class="mono child-name">{{ child.name }}</code>
              <el-tag v-if="child.status" size="small" :type="child.status === 'okay' ? 'success' : 'info'" effect="plain">{{ child.status }}</el-tag>
              <span v-if="child.compatible" class="node-compat">{{ child.compatible }}</span>
            </div>
          </div>
          <div v-else class="empty-children">无子节点</div>
        </el-collapse-item>
      </el-collapse>
    </section>

    <!-- DTBO 文件详情查看器 -->
    <section class="glass-card" v-if="dtbFiles.length && dtbFiles.some(f => f.endsWith('.dtbo'))">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 00-2 2v16a2 2 0 002 2h12a2 2 0 002-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/></svg>
          DTBO 文件查看器
        </div>
      </div>
      <div class="dtbo-selector">
        <el-select v-model="selectedDtbo" placeholder="选择 DTBO 文件查看详情" size="default" filterable clearable style="width: 100%; max-width: 400px;">
          <el-option v-for="f in dtboFileList" :key="f" :label="f" :value="f" />
        </el-select>
        <el-button :icon="View" type="primary" @click="fetchDtboDetail" :loading="dtboLoading" :disabled="!selectedDtbo">查看</el-button>
      </div>
      <div v-if="dtboDetail" class="dtbo-result">
        <div class="dtbo-meta">
          <el-tag effect="plain" size="small">{{ dtboDetail.file }}</el-tag>
          <el-tag effect="plain" size="small" type="info">{{ fmtBytes(dtboDetail.size) }}</el-tag>
          <el-tag v-if="dtboDetail.dts_available" effect="plain" size="small" type="success">dtc 反编译成功</el-tag>
          <el-tag v-else effect="plain" size="small" type="danger">dtc 不可用</el-tag>
        </div>
        <div v-if="dtboDetail.fragments.length" class="dtbo-fragments">
          <div class="frag-title">Fragments 摘要</div>
          <div v-for="(frag, idx) in dtboDetail.fragments" :key="idx" class="frag-item">
            <code class="mono">{{ frag.fragment }}</code>
            <span class="frag-target">{{ frag.target_line }}</span>
          </div>
        </div>
        <el-collapse v-if="dtboDetail.dts_available">
          <el-collapse-item title="DTS 反编译内容" name="dts">
            <pre class="terminal dts-terminal">{{ dtboDetail.dts_content }}</pre>
          </el-collapse-item>
        </el-collapse>
        <div v-if="dtboDetail.dts_error" class="dts-error">
          <el-alert :title="dtboDetail.dts_error" type="error" :closable="false" />
        </div>
      </div>
    </section>

    <!-- 原始 config.txt -->
    <section class="glass-card" v-if="config.config_raw">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="16 18 22 12 16 6"/><polyline points="8 6 2 12 8 18"/></svg>
          config.txt 原始内容
        </div>
      </div>
      <pre class="terminal">{{ config.config_raw }}</pre>
    </section>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Refresh, View } from '@element-plus/icons-vue'
import { systemApi, type BoardConfig, type DeviceTreeInfo, type DtboDetail } from '../../api'

const loading = ref(false)
const config = ref<Partial<BoardConfig>>({})
const dtInfo = ref<DeviceTreeInfo | null>(null)
const selectedDtbo = ref('')
const dtboLoading = ref(false)
const dtboDetail = ref<DtboDetail | null>(null)

const dtbFiles = computed(() => config.value.dtb_files || [])
const bootFiles = computed(() => config.value.boot_files || [])

const boardType = computed(() => {
  const bt = config.value.board_type || ''
  if (bt) return bt
  const model = config.value.board_model || ''
  return model || null
})

const SECTION_MAP: Record<string, string> = {
  'Ethernet Configuration': '网络配置',
  'Peripherals Configuration': '外设配置',
  'Boot Configuration': '启动配置',
  'Memory Configuration': '内存配置',
  'Frequency Configuration': '频率配置',
}

const PARAM_SECTIONS: Record<string, string> = {
  ota_serverip: '网络配置', serverip: '网络配置', ethact: '网络配置',
  force_pxe: '网络配置', nfs: '网络配置', rootpath: '网络配置',

  dtoverlay: '外设配置', dtparam: '外设配置', eeprom: '外设配置',
  sata: '外设配置', wifi: '外设配置', bt: '外设配置',
  '4g_5g_modem': '外设配置', board_power_monitor: '外设配置',
  watchdog0: '外设配置', eeprom_write_protect: '外设配置',

  boot_mode: '启动配置', boot_fitconfig: '启动配置', firstboot: '启动配置',
  autoreboot: '启动配置', bootdelay: '启动配置', ubiparts: '启动配置',

  tacosys_mem_addr: '内存配置', tacosys_mem_size: '内存配置',
  npu_mem_addr: '内存配置', npu_mem_size: '内存配置', total_mem: '内存配置',

  cpu_max_freq: '频率配置', cpu_min_freq: '频率配置',
  dec_freq: '频率配置', enc_freq: '频率配置', npu_freq: '频率配置',
  cpu_freq: '频率配置', ddr_freq: '频率配置',
  ddr_board_id0: '频率配置', ddr_board_id1: '频率配置',
  ddr_port6_priority: '频率配置',
}

const groupedActiveParams = computed(() => {
  const params = config.value.active_params || {}
  const groups: Record<string, Record<string, string>> = {}

  for (const [key, val] of Object.entries(params)) {
    const section = PARAM_SECTIONS[key] || '其他配置'
    if (!groups[section]) groups[section] = {}
    groups[section][key] = val
  }

  const order = ['启动配置', '网络配置', '外设配置', '内存配置', '频率配置', '其他配置']
  const sorted: Record<string, Record<string, string>> = {}
  for (const s of order) {
    if (groups[s]) sorted[s] = groups[s]
  }
  for (const [s, g] of Object.entries(groups)) {
    if (!sorted[s]) sorted[s] = g
  }
  return sorted
})

const heroStats = computed(() => {
  const p = config.value.active_params || {}
  const stats = []
  if (p.boot_mode) stats.push({ label: '启动模式', value: p.boot_mode })
  if (p.cpu_max_freq) stats.push({ label: 'CPU 最大频率', value: fmtFreq(p.cpu_max_freq) })
  if (p.ethact) stats.push({ label: '网络接口', value: p.ethact })
  if (p.dtoverlay) stats.push({ label: 'DT Overlay', value: p.dtoverlay.replace('.dtbo', '') })
  if ((config.value.dtb_files || []).length > 0) stats.push({ label: 'DTB 文件', value: `${config.value.dtb_files!.length} 个` })
  return stats
})

function paramClass(key: string, val: string) {
  if (val === 'true' || val === 'on') return 'val-on'
  if (val === 'false' || val === 'off') return 'val-off'
  if (/^0x[0-9a-f]+$/i.test(val)) return 'val-hex'
  if (/^\d+$/.test(val) && parseInt(val) > 1000000) return 'val-freq'
  return ''
}

function formatValue(key: string, val: string) {
  if (key.includes('freq') && /^\d+$/.test(val)) return fmtFreq(val)
  if (key.includes('mem_size') && /^0x[0-9a-f]+$/i.test(val)) {
    const bytes = parseInt(val, 16)
    return `${val} (${(bytes / 1024 / 1024 / 1024).toFixed(1)} GiB)`
  }
  return val
}

function fmtFreq(hz: string) {
  const n = parseInt(hz)
  if (isNaN(n)) return hz
  if (n >= 1e9) return `${(n / 1e9).toFixed(2)} GHz`
  if (n >= 1e6) return `${(n / 1e6).toFixed(0)} MHz`
  if (n >= 1e3) return `${(n / 1e3).toFixed(0)} KHz`
  return `${n} Hz`
}

function fmtBytes(bytes?: number) {
  if (bytes === undefined || bytes === null) return '--'
  if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${bytes} B`
}

const dtboFileList = computed(() =>
  (config.value.dtb_files || []).filter(f => f.endsWith('.dtbo'))
)

function isActiveOverride(name: string) {
  const params = config.value.active_params || {}
  return name in params
}

async function fetchConfig() {
  loading.value = true
  try {
    const [cfgRes, dtRes] = await Promise.allSettled([
      systemApi.boardConfig(),
      systemApi.deviceTree(),
    ])
    if (cfgRes.status === 'fulfilled') config.value = cfgRes.value.data.data
    if (dtRes.status === 'fulfilled') dtInfo.value = dtRes.value.data.data
  } catch { /* ignore */ }
  loading.value = false
}

async function fetchDtboDetail() {
  if (!selectedDtbo.value) return
  dtboLoading.value = true
  dtboDetail.value = null
  try {
    const res = await systemApi.dtboDetail(selectedDtbo.value)
    dtboDetail.value = res.data.data
  } catch { /* ignore */ }
  dtboLoading.value = false
}

onMounted(() => fetchConfig())
</script>

<style scoped>
.board-tab { display: flex; flex-direction: column; gap: 16px; }

/* Hero */
.board-hero {
  position: relative; border-radius: 16px; overflow: hidden;
  background: linear-gradient(135deg, #1a0f2e 0%, #2d1b4e 50%, #3d2566 100%);
}
.hero-bg {
  position: absolute; inset: 0;
  background:
    radial-gradient(ellipse 60% 50% at 80% 20%, rgba(139,92,246,0.15), transparent),
    radial-gradient(ellipse 40% 60% at 10% 80%, rgba(236,72,153,0.10), transparent);
  pointer-events: none;
}
.hero-content {
  position: relative; display: flex; align-items: center; gap: 24px;
  padding: 28px 32px; color: #fff; flex-wrap: wrap;
}
.hero-left { flex: 1; min-width: 200px; }
.hero-chip {
  display: inline-block; font-size: 11px; font-weight: 700; letter-spacing: 2px;
  text-transform: uppercase; padding: 3px 10px; border-radius: 20px;
  background: rgba(139,92,246,0.2); border: 1px solid rgba(139,92,246,0.35);
  color: #c4b5fd; margin-bottom: 8px;
}
.hero-ver { font-size: 28px; font-weight: 800; letter-spacing: 0.5px; line-height: 1.2; }
.hero-sub { font-size: 12px; color: rgba(255,255,255,0.45); margin-top: 4px; word-break: break-all; }
.hero-right { display: flex; gap: 16px; flex-wrap: wrap; }
.hero-stat {
  display: flex; flex-direction: column; align-items: center; min-width: 68px;
  background: rgba(255,255,255,0.06); border-radius: 10px; padding: 8px 12px;
  border: 1px solid rgba(255,255,255,0.08);
}
.hs-val { font-size: 13px; font-weight: 700; color: #e0e7ff; text-align: center; }
.hs-lbl { font-size: 9px; color: rgba(255,255,255,0.45); margin-top: 2px; text-transform: uppercase; letter-spacing: 0.5px; }
.hero-refresh { position: absolute; top: 16px; right: 16px; color: rgba(255,255,255,0.6) !important; }

/* Glass card */
.glass-card {
  background: #fff; border-radius: 14px; padding: 18px 22px;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04), 0 4px 16px rgba(0,0,0,0.02);
  border: 1px solid #f3f4f6;
}
.gc-header {
  display: flex; justify-content: space-between; align-items: center;
  margin-bottom: 14px; flex-wrap: wrap; gap: 8px;
}
.gc-title {
  display: flex; align-items: center; gap: 8px;
  font-size: 15px; font-weight: 700; color: #111827;
}
.gc-icon { width: 18px; height: 18px; color: #8b5cf6; flex-shrink: 0; }

/* Param groups */
.param-groups { display: flex; flex-direction: column; gap: 16px; }
.param-group-title {
  font-size: 13px; font-weight: 600; color: #6b7280;
  border-bottom: 1px solid #f3f4f6; padding-bottom: 4px; margin-bottom: 8px;
}
.param-grid {
  display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 8px;
}
.param-item {
  display: flex; justify-content: space-between; align-items: center;
  padding: 8px 12px; background: #fafbfc; border-radius: 8px;
  border: 1px solid #f0f1f3; font-size: 13px;
}
.param-item.commented { opacity: 0.6; }
.param-key { font-weight: 600; color: #374151; font-family: 'JetBrains Mono', monospace; font-size: 12px; }
.param-val { color: #111827; font-weight: 500; text-align: right; max-width: 60%; word-break: break-all; }
.param-val.muted { color: #9ca3af; }
.val-on { color: #059669; font-weight: 700; }
.val-off { color: #dc2626; font-weight: 700; }
.val-hex { color: #7c3aed; font-family: 'JetBrains Mono', monospace; font-size: 12px; }
.val-freq { color: #2563eb; }

/* DTB tags */
.dtb-tags { display: flex; flex-wrap: wrap; gap: 6px; }
.dtb-tag { font-family: 'JetBrains Mono', monospace; font-size: 12px; }

/* Boot file table */
.boot-table :deep(.el-table__row td) { font-size: 13px; }

/* Terminal */
.terminal {
  background: #111827; color: #d1d5db; padding: 12px 14px; border-radius: 8px;
  font-family: 'JetBrains Mono','Courier New',monospace; font-size: 12px;
  line-height: 1.6; max-height: 360px; overflow: auto; white-space: pre; margin: 0;
}

/* Overrides table */
.gc-desc { font-size: 12px; color: #9ca3af; margin: -8px 0 12px 0; }
.mono { font-family: 'JetBrains Mono', monospace; font-size: 12px; }
.node-path { color: #2563eb; }
.prop-name { color: #7c3aed; }
.hex-val { color: #9ca3af; font-size: 11px; }
.active-badge { margin-left: 6px; }
.override-table :deep(.el-table__row td) { font-size: 13px; }

/* Node tree */
.node-title { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.node-compat { font-size: 11px; color: #9ca3af; }
.node-children { display: flex; flex-direction: column; gap: 4px; }
.child-item {
  display: flex; align-items: center; gap: 8px;
  padding: 4px 8px; background: #f9fafb; border-radius: 6px; font-size: 13px;
}
.child-name { color: #374151; }
.empty-children { color: #9ca3af; font-size: 12px; padding: 4px 0; }

/* DTBO viewer */
.dtbo-selector { display: flex; gap: 8px; align-items: center; margin-bottom: 12px; }
.dtbo-result { display: flex; flex-direction: column; gap: 10px; }
.dtbo-meta { display: flex; gap: 6px; flex-wrap: wrap; }
.dtbo-fragments { display: flex; flex-direction: column; gap: 4px; }
.frag-title { font-size: 13px; font-weight: 600; color: #374151; margin-bottom: 4px; }
.frag-item {
  display: flex; align-items: center; gap: 10px; padding: 6px 10px;
  background: #fffbeb; border-radius: 6px; border: 1px solid #fde68a; font-size: 13px;
}
.frag-target { color: #92400e; font-size: 12px; }
.dts-terminal { max-height: 500px; }
.dts-error { margin-top: 4px; }
</style>
