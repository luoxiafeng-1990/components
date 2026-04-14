<template>
  <div class="sys-tab">
    <!-- ═══════ Hero: TACO 版本 ═══════ -->
    <div class="hero">
      <div class="hero-bg"></div>
      <div class="hero-content">
        <div class="hero-left">
          <div class="hero-chip">TACO Platform</div>
          <div class="hero-ver">{{ tacoVersion }}</div>
          <div class="hero-sub" v-if="info.kernel">{{ info.arch }} · Linux {{ info.kernel }}</div>
        </div>
        <div class="hero-right">
          <div class="hero-stat" v-for="s in heroStats" :key="s.label">
            <span class="hs-val">{{ s.value }}</span>
            <span class="hs-lbl">{{ s.label }}</span>
          </div>
        </div>
        <el-button class="hero-refresh" :icon="Refresh" circle @click="fetchAll" :loading="loading" />
      </div>
    </div>

    <!-- ═══════ 版本明细条 ═══════ -->
    <div class="detail-strip" v-if="versionItems.length">
      <div v-for="item in versionItems" :key="item.label" class="ds-item">
        <span class="ds-dot" :style="{ background: item.color }"></span>
        <span class="ds-label">{{ item.label }}</span>
        <span class="ds-value">{{ item.value }}</span>
      </div>
    </div>

    <!-- ═══════ 文件系统 ═══════ -->
    <section class="glass-card">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="18" rx="2"/><path d="M2 9h20M9 21V9"/></svg>
          磁盘存储
        </div>
        <div class="gc-actions">
          <span class="gc-hint">每 60s 自动刷新</span>
          <el-button size="small" text type="primary" @click="fetchFs" :loading="fsLoading">
            <el-icon><Refresh /></el-icon>
          </el-button>
        </div>
      </div>
      <div v-if="partitions.length" class="fs-grid">
        <div v-for="p in partitions" :key="p.mount" class="fs-tile">
          <div class="fs-tile-header">
            <span class="fs-tile-mount">{{ p.mount || p.device }}</span>
            <span class="fs-tile-pct" :style="{ color: barColor(p.percent) }">{{ p.percent }}%</span>
          </div>
          <div class="fs-ring-wrap">
            <svg class="fs-ring" viewBox="0 0 80 80">
              <circle cx="40" cy="40" r="34" fill="none" stroke="#f0f1f3" stroke-width="6"/>
              <circle cx="40" cy="40" r="34" fill="none" :stroke="barColor(p.percent)" stroke-width="6"
                stroke-linecap="round" :stroke-dasharray="`${p.percent * 2.136} 213.6`"
                transform="rotate(-90 40 40)" style="transition: stroke-dasharray .6s ease"/>
            </svg>
            <div class="fs-ring-text">
              <span class="fs-ring-used">{{ fmtMB(p.used_mb) }}</span>
              <span class="fs-ring-total">/ {{ fmtMB(p.size_mb) }}</span>
            </div>
          </div>
          <div class="fs-tile-dev">{{ p.device }}</div>
        </div>
      </div>
      <el-empty v-else description="加载中..." :image-size="40" />
      <details v-if="lsblkRaw" class="lsblk-details">
        <summary>块设备详情</summary>
        <pre class="terminal">{{ lsblkRaw }}</pre>
      </details>
    </section>

    <!-- ═══════ 厂商 APT 数据源 ═══════ -->
    <section class="glass-card">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 01-2 2H5a2 2 0 01-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
          APT 数据源管理
        </div>
        <el-button type="primary" size="small" @click="updateSource" :loading="srcUpdating">
          <el-icon style="margin-right:4px"><Refresh /></el-icon>写入并更新
        </el-button>
      </div>
      <div class="src-hint">内容将写入远程 <code>/etc/apt/sources.list</code>，执行 <code>apt-get update</code> 后拉取所有可用包</div>
      <el-input
        v-model="vendorSource"
        type="textarea"
        :rows="3"
        placeholder="deb http://... noble main"
        class="src-textarea"
      />
      <el-alert v-if="srcMsg" :title="srcMsg" :type="srcOk ? 'success' : 'error'" show-icon closable @close="srcMsg=''" style="margin-top:8px" />
    </section>

    <!-- ═══════ 已安装包 ═══════ -->
    <div class="pkg-row-2col">
      <section class="glass-card">
        <div class="gc-header">
          <div class="gc-title">
            <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2L2 7l10 5 10-5-10-5z"/><path d="M2 17l10 5 10-5"/><path d="M2 12l10 5 10-5"/></svg>
            厂商已安装包
          </div>
          <el-tag size="small" effect="dark" type="primary" round>{{ vendorInstalled.length }}</el-tag>
        </div>
        <el-table :data="vendorInstalled" size="small" stripe max-height="320" empty-text="无" class="pkg-table">
          <el-table-column prop="name" label="包名" min-width="180" sortable show-overflow-tooltip />
          <el-table-column prop="version" label="版本" min-width="160" show-overflow-tooltip />
        </el-table>
      </section>
      <section class="glass-card">
        <div class="gc-header">
          <div class="gc-title">
            <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="18" height="18" rx="2"/><path d="M3 9h18M9 21V9"/></svg>
            系统已安装包
          </div>
          <el-tag size="small" effect="plain" type="info" round>{{ systemInstalled.length }}</el-tag>
        </div>
        <el-input v-model="sysSearch" placeholder="搜索..." clearable size="small" style="margin-bottom:8px" />
        <el-table :data="filteredSys" size="small" stripe max-height="280" empty-text="无" class="pkg-table">
          <el-table-column prop="name" label="包名" min-width="180" sortable show-overflow-tooltip />
          <el-table-column prop="version" label="版本" min-width="160" show-overflow-tooltip />
        </el-table>
      </section>
    </div>

    <!-- ═══════ 软件包浏览器 ═══════ -->
    <section class="glass-card">
      <div class="gc-header">
        <div class="gc-title">
          <svg class="gc-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M16 4h2a2 2 0 012 2v14a2 2 0 01-2 2H6a2 2 0 01-2-2V6a2 2 0 012-2h2"/><rect x="8" y="2" width="8" height="4" rx="1"/></svg>
          软件包浏览器
        </div>
        <div class="gc-actions">
          <el-radio-group v-model="pkgFilter" size="small">
            <el-radio-button value="vendor">厂商包</el-radio-button>
            <el-radio-button value="all">全部</el-radio-button>
          </el-radio-group>
          <el-button size="small" text type="primary" @click="fetchDebs" :loading="debLoading">
            <el-icon><Refresh /></el-icon>
          </el-button>
        </div>
      </div>
      <el-input v-model="debSearch" placeholder="搜索包名..." clearable prefix-icon="Search" style="margin-bottom:12px" />
      <div v-if="Object.keys(filteredPkgs).length === 0" class="pkg-empty">
        <el-empty description="暂无包数据，请先「写入并更新」数据源" :image-size="60" />
      </div>
      <el-collapse v-else accordion>
        <el-collapse-item v-for="(pkg, name) in filteredPkgs" :key="String(name)" :name="String(name)">
          <template #title>
            <div class="pkg-title-row">
              <span class="pkg-name-text">{{ name }}</span>
              <el-tag size="small" round>{{ pkg.versions?.length || 0 }} 版本</el-tag>
              <span class="pkg-desc-text">{{ pkg.description }}</span>
            </div>
          </template>
          <el-table :data="pkg.versions || []" size="small" stripe>
            <el-table-column prop="version" label="版本" min-width="220" />
            <el-table-column prop="status" label="状态" width="100">
              <template #default="{ row }">
                <el-tag :type="row.status === 'installed' ? 'success' : ''" size="small" effect="light" round>
                  {{ row.status === 'installed' ? '已安装' : '可安装' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="100">
              <template #default="{ row }">
                <el-button v-if="row.status !== 'installed'" type="primary" size="small" link
                  @click="installPkg(String(name), row.version)"
                  :loading="installing === `${name}=${row.version}`">安装</el-button>
              </template>
            </el-table-column>
          </el-table>
        </el-collapse-item>
      </el-collapse>
    </section>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { Refresh } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { systemApi, type SystemInfo } from '../../api'

interface Partition {
  device: string; mount: string
  size_mb: number; used_mb: number; avail_mb: number; percent: number
}

const DEFAULT_SRC = 'deb http://172.16.1.193:6520/ubuntu noble main\ndeb https://mirrors.aliyun.com/ubuntu-ports noble main universe'
const VENDOR_RE = /^(taco|tps|tacv|ta-|libdec|libenc|libnpu|viplite|webui)/i

const loading = ref(false)
const fsLoading = ref(false)
const debLoading = ref(false)
const srcUpdating = ref(false)
const info = ref<Partial<SystemInfo>>({})
const debData = ref<Record<string, any>>({})
const partitions = ref<Partition[]>([])
const lsblkRaw = ref('')
const debSearch = ref('')
const sysSearch = ref('')
const installing = ref('')
const vendorSource = ref(DEFAULT_SRC)
const srcMsg = ref('')
const srcOk = ref(false)
const pkgFilter = ref<'vendor' | 'all'>('vendor')
let fsTimer: ReturnType<typeof setInterval> | null = null

function barColor(pct: number) { return pct > 90 ? '#ef4444' : pct > 70 ? '#f59e0b' : '#3b82f6' }

function fmtMB(mb: number) {
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)}G`
  return `${mb}M`
}

const tacoVersion = computed(() => {
  const raw = info.value.tps_version || ''
  const m = raw.match(/TACO\s+version:\s*(\S+)/i)
  return m ? m[1] : raw.split(/\s+/).slice(0, 3).join(' ') || '--'
})

const heroStats = computed(() => {
  const items = []
  if (info.value.hostname) items.push({ label: '主机名', value: info.value.hostname })
  if (info.value.cpu_cores) items.push({ label: 'CPU 核心', value: `${info.value.cpu_cores} 核` })
  if (info.value.board_model) items.push({ label: '板卡', value: info.value.board_model })
  if (info.value.uptime_seconds) items.push({ label: '运行', value: fmtUptime(info.value.uptime_seconds) })
  return items
})

const versionItems = computed(() => {
  const raw = info.value.tps_version || ''
  if (!raw) return []
  const items: { label: string; value: string; color: string }[] = []
  const patterns: [string, RegExp, string][] = [
    ['FFmpeg', /FFmpeg:\s*(\S+)/i, '#8b5cf6'],
    ['OpenCV', /OpenCV:\s*(\S+)/i, '#06b6d4'],
    ['U-Boot', /U-Boot:\s*(U-Boot\s+\S+)/i, '#f97316'],
    ['内核', /Kernel\s+version:\s*Linux\s+\S+\s+(\S+)/i, '#10b981'],
    ['硬件', /HWversion:\s*(.+?)(?=\s+MCU|$)/i, '#ec4899'],
    ['MCU', /MCUversion[^:]*:\s*(\S+)/i, '#6366f1'],
  ]
  for (const [label, re, color] of patterns) {
    const match = raw.match(re)
    if (match) items.push({ label, value: match[1].trim(), color })
  }
  return items
})

const allInstalled = computed(() => {
  const pkgs = debData.value.packages || {}
  const list: { name: string; version: string; description: string }[] = []
  for (const [name, data] of Object.entries(pkgs) as [string, any][]) {
    const installed = (data.versions || []).find((v: any) => v.status === 'installed')
    if (installed) list.push({ name, version: installed.version, description: data.description || '' })
  }
  return list.sort((a, b) => a.name.localeCompare(b.name))
})

const vendorInstalled = computed(() => allInstalled.value.filter(p => VENDOR_RE.test(p.name)))
const systemInstalled = computed(() => allInstalled.value.filter(p => !VENDOR_RE.test(p.name)))

const filteredSys = computed(() => {
  if (!sysSearch.value) return systemInstalled.value
  const s = sysSearch.value.toLowerCase()
  return systemInstalled.value.filter(p => p.name.toLowerCase().includes(s))
})

const filteredPkgs = computed(() => {
  const allPkgs = debData.value.packages || {}
  let pool: Record<string, any>
  if (pkgFilter.value === 'vendor') {
    pool = {}
    for (const [n, d] of Object.entries(allPkgs)) {
      if (VENDOR_RE.test(n)) pool[n] = d
    }
  } else {
    pool = allPkgs
  }
  if (!debSearch.value) return pool
  const s = debSearch.value.toLowerCase()
  const r: Record<string, any> = {}
  for (const [n, d] of Object.entries(pool)) { if (n.toLowerCase().includes(s)) r[n] = d }
  return r
})

function fmtUptime(s: number) {
  if (!s) return '--'
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60)
  if (d > 0) return `${d}天${h}时`
  return h > 0 ? `${h}时${m}分` : `${m}分`
}

async function fetchInfo() { try { info.value = (await systemApi.info()).data.data } catch { /* */ } }

async function fetchFs() {
  fsLoading.value = true
  try {
    const d = (await systemApi.filesystem()).data.data
    if (Array.isArray(d.partitions)) {
      partitions.value = d.partitions
    } else if (typeof d.df === 'string') {
      const lines = d.df.split('\n').filter((l: string) => l && !l.startsWith('Filesystem'))
      partitions.value = lines.map((line: string) => {
        const cols = line.split(/\s+/)
        const parseSz = (s: string) => {
          if (!s) return 0
          const n = parseFloat(s)
          if (s.endsWith('G')) return Math.round(n * 1024)
          if (s.endsWith('T')) return Math.round(n * 1024 * 1024)
          if (s.endsWith('M')) return Math.round(n)
          if (s.endsWith('K')) return Math.round(n / 1024)
          return Math.round(n)
        }
        return {
          device: cols[0] || '', mount: cols[5] || cols[cols.length - 1] || '',
          size_mb: parseSz(cols[1]), used_mb: parseSz(cols[2]),
          avail_mb: parseSz(cols[3]), percent: parseInt(cols[4]) || 0,
        } as Partition
      }).filter((p: Partition) => p.device.startsWith('/dev/') || p.device === 'tmpfs' || p.device === 'overlay')
    }
    lsblkRaw.value = d.lsblk || ''
  } catch {
    partitions.value = []
    lsblkRaw.value = ''
  }
  fsLoading.value = false
}

async function fetchDebs() {
  debLoading.value = true
  try { debData.value = (await systemApi.debs()).data.data } catch { /* */ }
  debLoading.value = false
}

async function fetchSource() {
  try {
    const r = await systemApi.aptSource()
    const src = r.data.data?.source
    if (src) vendorSource.value = src
  } catch { /* use default */ }
}

async function fetchAll() {
  loading.value = true
  await Promise.all([fetchInfo(), fetchFs(), fetchDebs(), fetchSource()])
  loading.value = false
}

async function updateSource() {
  srcUpdating.value = true; srcMsg.value = ''
  try {
    const r = await systemApi.updateAptSource(vendorSource.value)
    srcMsg.value = r.data.message || '更新成功'; srcOk.value = true
    if (r.data.data?.packages) {
      debData.value = { packages: r.data.data.packages }
    } else {
      await fetchDebs()
    }
  } catch (e: any) { srcMsg.value = e.message || '更新失败'; srcOk.value = false }
  srcUpdating.value = false
}

async function installPkg(pkg: string, ver: string) {
  try { await ElMessageBox.confirm(`确认安装 ${pkg} (${ver})?`, '安装', { type: 'warning' }) } catch { return }
  installing.value = `${pkg}=${ver}`
  try {
    const r = await systemApi.installDeb(pkg, ver)
    ElMessage.success(r.data.message || '安装成功'); await fetchDebs()
  } catch (e: any) { ElMessage.error(e.message || '安装失败') }
  installing.value = ''
}

onMounted(() => {
  fetchAll()
  fsTimer = setInterval(fetchFs, 60000)
})
onUnmounted(() => { if (fsTimer) clearInterval(fsTimer) })
</script>

<style scoped>
.sys-tab { display: flex; flex-direction: column; gap: 16px; }

/* ══ Hero ══ */
.hero {
  position: relative; border-radius: 16px; overflow: hidden;
  background: linear-gradient(135deg, #0c1929 0%, #162544 50%, #1e3a5f 100%);
}
.hero-bg {
  position: absolute; inset: 0;
  background:
    radial-gradient(ellipse 60% 50% at 80% 20%, rgba(59,130,246,0.15), transparent),
    radial-gradient(ellipse 40% 60% at 10% 80%, rgba(139,92,246,0.10), transparent);
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
  background: rgba(59,130,246,0.2); border: 1px solid rgba(59,130,246,0.35);
  color: #93c5fd; margin-bottom: 8px;
}
.hero-ver { font-size: 36px; font-weight: 800; letter-spacing: 0.5px; line-height: 1.1; }
.hero-sub { font-size: 13px; color: rgba(255,255,255,0.5); margin-top: 6px; }
.hero-right { display: flex; gap: 20px; flex-wrap: wrap; }
.hero-stat {
  display: flex; flex-direction: column; align-items: center; min-width: 72px;
  background: rgba(255,255,255,0.06); border-radius: 10px; padding: 10px 14px;
  border: 1px solid rgba(255,255,255,0.08);
}
.hs-val { font-size: 14px; font-weight: 700; color: #e0e7ff; }
.hs-lbl { font-size: 10px; color: rgba(255,255,255,0.45); margin-top: 2px; text-transform: uppercase; letter-spacing: 0.5px; }
.hero-refresh { position: absolute; top: 16px; right: 16px; color: rgba(255,255,255,0.6) !important; }

/* ══ Detail strip ══ */
.detail-strip {
  display: flex; flex-wrap: wrap; gap: 6px;
  padding: 0 4px;
}
.ds-item {
  display: flex; align-items: center; gap: 6px;
  background: #fff; border-radius: 8px; padding: 6px 12px;
  box-shadow: 0 1px 3px rgba(0,0,0,0.04);
  font-size: 13px; white-space: nowrap;
}
.ds-dot { width: 8px; height: 8px; border-radius: 50%; flex-shrink: 0; }
.ds-label { color: #6b7280; font-weight: 500; }
.ds-value { color: #111827; font-weight: 700; }

/* ══ Glass card ══ */
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
.gc-icon { width: 18px; height: 18px; color: #3b82f6; flex-shrink: 0; }
.gc-actions { display: flex; align-items: center; gap: 8px; }
.gc-hint { font-size: 11px; color: #9ca3af; }

/* ══ Filesystem ring tiles ══ */
.fs-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 12px; }
.fs-tile {
  display: flex; flex-direction: column; align-items: center;
  background: #fafbfc; border-radius: 12px; padding: 12px 10px 10px;
  border: 1px solid #f0f1f3;
  transition: box-shadow .2s;
}
.fs-tile:hover { box-shadow: 0 2px 12px rgba(0,0,0,0.06); }
.fs-tile-header { display: flex; justify-content: space-between; width: 100%; margin-bottom: 6px; }
.fs-tile-mount { font-size: 12px; font-weight: 600; color: #374151; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 80px; }
.fs-tile-pct { font-size: 13px; font-weight: 800; }
.fs-ring-wrap { position: relative; width: 80px; height: 80px; }
.fs-ring { width: 100%; height: 100%; }
.fs-ring-text { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; }
.fs-ring-used { font-size: 13px; font-weight: 700; color: #1f2937; line-height: 1.1; }
.fs-ring-total { font-size: 10px; color: #9ca3af; }
.fs-tile-dev { font-size: 10px; color: #9ca3af; margin-top: 4px; max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lsblk-details { margin-top: 14px; }
.lsblk-details summary {
  font-size: 12px; color: #6b7280; cursor: pointer; font-weight: 600;
  padding: 4px 8px; border-radius: 6px;
}
.lsblk-details summary:hover { background: #f3f4f6; }

/* ══ Source ══ */
.src-hint { font-size: 12px; color: #6b7280; margin-bottom: 8px; line-height: 1.5; }
.src-hint code {
  background: #f3f4f6; padding: 1px 5px; border-radius: 4px; font-size: 11px;
  font-family: 'JetBrains Mono','Courier New',monospace; color: #111827; font-weight: 600;
}
.src-textarea :deep(textarea) { font-family: 'JetBrains Mono','Courier New',monospace; font-size: 13px; }

/* ══ Packages ══ */
.pkg-row-2col { display: grid; grid-template-columns: repeat(auto-fit, minmax(380px, 1fr)); gap: 16px; }
.pkg-table :deep(.el-table__row td) { font-size: 13px; }
.pkg-empty { padding: 20px 0; }

.pkg-title-row { display: flex; align-items: center; width: 100%; gap: 8px; overflow: hidden; }
.pkg-name-text { font-weight: 700; min-width: 160px; font-size: 13px; color: #1f2937; }
.pkg-desc-text { color: #9ca3af; font-size: 12px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1; }

.terminal {
  background: #111827; color: #d1d5db; padding: 12px 14px; border-radius: 8px;
  font-family: 'JetBrains Mono','Courier New',monospace; font-size: 12px;
  line-height: 1.6; max-height: 220px; overflow: auto; white-space: pre; margin: 8px 0 0;
}
</style>
