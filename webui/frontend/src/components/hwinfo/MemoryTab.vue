<template>
  <div class="mem-tab">
    <el-alert v-if="errorMsg" :title="errorMsg" type="error" show-icon :closable="false" />
    <el-row :gutter="16">
      <el-col :span="10">
        <el-card shadow="never" class="card">
          <template #header>
            <div class="card-hdr">
              <span>内存使用分布</span>
              <span class="badge" :class="memPct > 80 ? 'danger' : memPct > 60 ? 'warning' : 'normal'">
                {{ memPct.toFixed(1) }}%
              </span>
            </div>
          </template>
          <v-chart v-if="hasData" :option="pieOpt" autoresize style="height: 260px;" />
          <el-empty v-else description="等待数据..." :image-size="60" />
        </el-card>
      </el-col>
      <el-col :span="14">
        <div class="stat-grid">
          <div class="mem-card total"><div class="mc-label">总内存</div><div class="mc-value">{{ fmtMB(memTotal) }}</div></div>
          <div class="mem-card used"><div class="mc-label">已使用</div><div class="mc-value">{{ fmtMB(memUsed) }}</div></div>
          <div class="mem-card avail"><div class="mc-label">可用</div><div class="mc-value">{{ fmtMB(memAvail) }}</div></div>
          <div class="mem-card cached"><div class="mc-label">缓存</div><div class="mc-value">{{ fmtMB(memCached) }}</div></div>
          <div class="mem-card buffers"><div class="mc-label">Buffers</div><div class="mc-value">{{ fmtMB(memBuffers) }}</div></div>
          <div class="mem-card free"><div class="mc-label">空闲</div><div class="mc-value">{{ fmtMB(memFree) }}</div></div>
        </div>
      </el-col>
    </el-row>

    <el-card shadow="never" class="card">
      <template #header><div class="card-hdr"><span>内存使用趋势</span></div></template>
      <v-chart v-if="hasData" :option="trendOpt" autoresize style="height: 180px;" />
      <el-empty v-else description="等待数据..." :image-size="60" />
    </el-card>

    <el-card shadow="never" class="card">
      <template #header>
        <div class="card-hdr">
          <span>DMA / CMA 内存池</span>
          <el-button size="small" @click="fetchDma" :loading="dmaLoading">刷新</el-button>
        </div>
      </template>
      <div v-if="dmaData.cma_total" class="stat-grid" style="margin-bottom: 12px;">
        <div class="mem-card cma"><div class="mc-label">CMA 总量</div><div class="mc-value">{{ fmtKB(dmaData.cma_total) }}</div></div>
        <div class="mem-card cma"><div class="mc-label">CMA 空闲</div><div class="mc-value">{{ fmtKB(dmaData.cma_free) }}</div></div>
        <div class="mem-card cma"><div class="mc-label">CMA 已用</div><div class="mc-value">{{ fmtKB(parseInt(dmaData.cma_total||'0') - parseInt(dmaData.cma_free||'0')) }}</div></div>
      </div>
      <template v-for="(val, key) in dmaBlocks" :key="key">
        <div v-if="val" class="info-block">
          <div class="info-title">{{ key }}</div>
          <pre class="terminal">{{ val }}</pre>
        </div>
      </template>
      <el-empty v-if="!dmaHasContent" description="DMA/CMA 信息不可用（需后端重新编译）" :image-size="60" />
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { PieChart, LineChart } from 'echarts/charts'
import { GridComponent, TooltipComponent, LegendComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import { systemApi } from '../../api'

use([PieChart, LineChart, CanvasRenderer, GridComponent, TooltipComponent, LegendComponent])

const MAX = 60
const hasData = ref(false)
const errorMsg = ref('')
const memTotal = ref(0)
const memUsed = ref(0)
const memFree = ref(0)
const memAvail = ref(0)
const memCached = ref(0)
const memBuffers = ref(0)
const memPct = ref(0)
const pieOpt = ref<Record<string, any>>({})
const trendOpt = ref<Record<string, any>>({})
const dmaLoading = ref(false)
const dmaData = ref<Record<string, any>>({})

const labels: string[] = []
const usedH: number[] = []
const cachedH: number[] = []
const buffersH: number[] = []
let timer: ReturnType<typeof setInterval> | null = null
let polling = false

function fmtMB(mb: number) { return mb >= 1024 ? `${(mb / 1024).toFixed(1)} GB` : `${mb} MB` }
function fmtKB(kb: number | string) {
  const v = typeof kb === 'string' ? parseInt(kb) || 0 : kb
  if (v >= 1048576) return `${(v / 1048576).toFixed(1)} GB`
  if (v >= 1024) return `${(v / 1024).toFixed(0)} MB`
  return `${v} kB`
}

const dmaHasContent = computed(() => {
  const d = dmaData.value
  return d.cma_total || d.tps_smi_memory || d.umap_media_mem || d.dma_buf
})
const dmaBlocks = computed(() => {
  const d = dmaData.value
  const r: Record<string, string> = {}
  if (d.tps_smi_memory) r['厂商内存池 (tps-smi)'] = d.tps_smi_memory
  if (d.umap_media_mem) r['Media Memory'] = d.umap_media_mem
  if (d.dma_buf) r['DMA Buffer'] = d.dma_buf
  return r
})

function rebuildCharts() {
  const actualUsed = Math.max(0, memUsed.value - memCached.value - memBuffers.value)
  pieOpt.value = {
    tooltip: { trigger: 'item', formatter: '{b}: {c} MB ({d}%)' },
    legend: { bottom: 0, textStyle: { fontSize: 11, color: '#909399' } },
    series: [{
      type: 'pie', radius: ['50%', '75%'], center: ['50%', '45%'],
      avoidLabelOverlap: true, label: { show: false },
      emphasis: { label: { show: true, fontSize: 14, fontWeight: 'bold' } },
      data: [
        { value: actualUsed, name: '已用', itemStyle: { color: '#409eff' } },
        { value: memCached.value, name: '缓存', itemStyle: { color: '#67c23a' } },
        { value: memBuffers.value, name: 'Buffers', itemStyle: { color: '#e6a23c' } },
        { value: memFree.value, name: '空闲', itemStyle: { color: '#dcdfe6' } },
      ]
    }]
  }
  trendOpt.value = {
    tooltip: { trigger: 'axis' },
    legend: { data: ['已用', '缓存', 'Buffers'], top: 0, textStyle: { fontSize: 11 } },
    grid: { left: 50, right: 12, top: 28, bottom: 22 },
    xAxis: { type: 'category', data: [...labels], boundaryGap: false, show: false },
    yAxis: { type: 'value', min: 0,
      axisLabel: { formatter: '{value}', fontSize: 11, color: '#aaa' },
      splitLine: { lineStyle: { color: '#f5f5f5' } } },
    series: [
      { name: '已用', type: 'line', data: [...usedH], smooth: true, symbol: 'none',
        lineStyle: { width: 2, color: '#409eff' },
        areaStyle: { color: { type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
          colorStops: [{ offset: 0, color: 'rgba(64,158,255,0.2)' }, { offset: 1, color: 'rgba(64,158,255,0.02)' }] } } },
      { name: '缓存', type: 'line', data: [...cachedH], smooth: true, symbol: 'none',
        lineStyle: { width: 1.5, color: '#67c23a' } },
      { name: 'Buffers', type: 'line', data: [...buffersH], smooth: true, symbol: 'none',
        lineStyle: { width: 1.5, color: '#e6a23c' } },
    ]
  }
}

async function poll() {
  if (polling) return
  polling = true
  try {
    const m = (await systemApi.metrics()).data.data
    hasData.value = true
    errorMsg.value = ''
    const t = new Date(m.timestamp).toLocaleTimeString('zh-CN', { hour12: false })
    labels.push(t); if (labels.length > MAX) labels.shift()
    memTotal.value = m.memory.total_mb
    memUsed.value = m.memory.used_mb
    memFree.value = m.memory.free_mb
    memAvail.value = m.memory.available_mb
    memCached.value = m.memory.cached_mb
    memBuffers.value = m.memory.buffers_mb
    memPct.value = m.memory.usage_percent
    usedH.push(m.memory.used_mb); if (usedH.length > MAX) usedH.shift()
    cachedH.push(m.memory.cached_mb); if (cachedH.length > MAX) cachedH.shift()
    buffersH.push(m.memory.buffers_mb); if (buffersH.length > MAX) buffersH.shift()
    rebuildCharts()
  } catch (error: any) {
    errorMsg.value = error?.message || '内存指标接口不可用'
  } finally {
    polling = false
  }
}

async function fetchDma() {
  dmaLoading.value = true
  try { dmaData.value = (await systemApi.dmaMem()).data.data } catch { /* */ }
  dmaLoading.value = false
}

onMounted(() => { poll(); setTimeout(poll, 800); timer = setInterval(poll, 2000); fetchDma() })
onUnmounted(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.mem-tab { display: flex; flex-direction: column; gap: 14px; }
.card { border-radius: 12px; }
.card :deep(.el-card__header) { padding: 10px 16px; border-bottom: 1px solid #f5f5f5; }
.card-hdr { display: flex; justify-content: space-between; align-items: center; font-weight: 600; font-size: 14px; }
.badge { font-size: 12px; font-weight: 600; padding: 2px 10px; border-radius: 10px; }
.badge.normal { background: #f0f9eb; color: #67c23a; }
.badge.warning { background: #fdf6ec; color: #e6a23c; }
.badge.danger { background: #fef0f0; color: #f56c6c; }
.stat-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }
.mem-card { border-radius: 10px; padding: 14px 16px; background: #f9fafb; border: 1px solid #f0f0f0; }
.mem-card.total { border-left: 3px solid #303133; }
.mem-card.used { border-left: 3px solid #409eff; }
.mem-card.avail { border-left: 3px solid #67c23a; }
.mem-card.cached { border-left: 3px solid #95d475; }
.mem-card.buffers { border-left: 3px solid #e6a23c; }
.mem-card.free { border-left: 3px solid #dcdfe6; }
.mem-card.cma { border-left: 3px solid #9b59b6; }
.mc-label { font-size: 12px; color: #909399; margin-bottom: 4px; }
.mc-value { font-size: 18px; font-weight: 700; color: #303133; }
.info-block { margin-bottom: 12px; }
.info-title { font-size: 13px; font-weight: 600; color: #606266; margin-bottom: 6px; padding-left: 8px; border-left: 3px solid #409eff; }
.terminal {
  background: #1a1a2e; color: #e0e0e0; padding: 12px 14px; border-radius: 8px;
  font-family: 'JetBrains Mono','Courier New',monospace; font-size: 12px;
  line-height: 1.6; max-height: 240px; overflow: auto; white-space: pre; margin: 0;
}
</style>
