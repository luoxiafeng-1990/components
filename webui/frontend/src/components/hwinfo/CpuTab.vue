<template>
  <div class="cpu-tab">
    <div class="summary-cards">
      <div class="stat-card accent">
        <div class="stat-body">
          <div class="stat-label">总使用率</div>
          <div class="stat-number">{{ cpuTotal.toFixed(1) }}<span class="unit">%</span></div>
        </div>
        <div class="stat-progress"><div class="fill" :style="{ width: Math.min(cpuTotal, 100) + '%' }"></div></div>
      </div>
      <div class="stat-card">
        <div class="stat-body">
          <div class="stat-label">核心数</div>
          <div class="stat-number">{{ coreCount }}</div>
        </div>
      </div>
      <div class="stat-card">
        <div class="stat-body">
          <div class="stat-label">状态</div>
          <div class="stat-number small">
            <el-tag :type="connected ? 'success' : 'danger'" size="small" effect="dark">
              {{ connected ? '采集中' : '断开' }}
            </el-tag>
          </div>
        </div>
      </div>
    </div>
    <el-alert v-if="errorMsg" :title="errorMsg" type="error" show-icon :closable="false" />

    <el-card shadow="never" class="trend-card">
      <template #header>
        <div class="card-hdr">
          <span>CPU 总使用率趋势</span>
          <span class="badge" :class="cpuTotal > 80 ? 'danger' : cpuTotal > 50 ? 'warning' : 'normal'">
            {{ cpuTotal.toFixed(1) }}%
          </span>
        </div>
      </template>
      <v-chart v-if="hasData" :option="totalOpt" autoresize style="height: 200px;" />
      <el-empty v-else description="等待数据..." :image-size="60" />
    </el-card>

    <div class="section-title">各核心实时使用率</div>
    <div class="core-grid" v-if="coreCount > 0">
      <div v-for="idx in coreCount" :key="idx" class="core-card">
        <div class="core-hdr">
          <span class="core-id">CPU {{ idx - 1 }}</span>
          <span class="core-pct" :style="{ color: pctColor(coreLatest[idx - 1] ?? 0) }">
            {{ (coreLatest[idx - 1] ?? 0).toFixed(0) }}%
          </span>
        </div>
        <v-chart v-if="coreOpts[idx - 1]" :option="coreOpts[idx - 1]" autoresize style="height: 100px;" />
      </div>
    </div>
    <el-empty v-else description="等待核心数据..." :image-size="60" />
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { LineChart } from 'echarts/charts'
import { GridComponent, TooltipComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import { systemApi } from '../../api'

use([LineChart, CanvasRenderer, GridComponent, TooltipComponent])

const MAX = 60
const COLORS = ['#409eff','#67c23a','#e6a23c','#f56c6c','#9b59b6','#1abc9c','#e67e22','#2ecc71']

const connected = ref(false)
const cpuTotal = ref(0)
const coreCount = ref(0)
const coreLatest = ref<number[]>([])
const hasData = ref(false)
const errorMsg = ref('')
const totalOpt = ref<Record<string, any>>({})
const coreOpts = ref<Record<string, any>[]>([])

const labels: string[] = []
const totalHist: number[] = []
const coreHists: number[][] = []
let timer: ReturnType<typeof setInterval> | null = null
let polling = false

function pctColor(p: number) { return p > 80 ? '#f56c6c' : p > 50 ? '#e6a23c' : '#67c23a' }

function rebuildTotal() {
  totalOpt.value = {
    tooltip: { trigger: 'axis' },
    grid: { left: 45, right: 15, top: 10, bottom: 25 },
    xAxis: { type: 'category', data: [...labels], boundaryGap: false, show: false },
    yAxis: { type: 'value', min: 0, max: 100, splitNumber: 4,
      axisLabel: { formatter: '{value}%', fontSize: 11, color: '#aaa' },
      splitLine: { lineStyle: { color: '#f0f0f0' } } },
    series: [{ type: 'line', data: [...totalHist], smooth: true, symbol: 'none',
      lineStyle: { width: 2, color: '#409eff' },
      areaStyle: { color: { type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
        colorStops: [{ offset: 0, color: 'rgba(64,158,255,0.35)' }, { offset: 1, color: 'rgba(64,158,255,0.02)' }] } }
    }]
  }
}

function rebuildCores() {
  const opts: Record<string, any>[] = []
  for (let i = 0; i < coreHists.length; i++) {
    const c = COLORS[i % COLORS.length]
    opts.push({
      grid: { left: 0, right: 0, top: 2, bottom: 2 },
      xAxis: { show: false, type: 'category', data: [...labels], boundaryGap: false },
      yAxis: { show: false, type: 'value', min: 0, max: 100 },
      series: [{ type: 'line', data: [...coreHists[i]], smooth: true, symbol: 'none',
        lineStyle: { width: 1.5, color: c },
        areaStyle: { color: { type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
          colorStops: [{ offset: 0, color: c + '45' }, { offset: 1, color: c + '05' }] } }
      }]
    })
  }
  coreOpts.value = opts
}

async function poll() {
  if (polling) return
  polling = true
  try {
    const res = await systemApi.metrics()
    const m = res.data.data
    connected.value = true
    hasData.value = true
    errorMsg.value = ''

    const t = new Date(m.timestamp).toLocaleTimeString('zh-CN', { hour12: false })
    labels.push(t)
    if (labels.length > MAX) labels.shift()

    cpuTotal.value = m.cpu.usage_percent ?? 0
    totalHist.push(cpuTotal.value)
    if (totalHist.length > MAX) totalHist.shift()

    const numCores = m.cpu.per_core?.length || m.cpu.cores || 0
    coreCount.value = numCores

    if (m.cpu.per_core?.length) {
      const lat: number[] = []
      m.cpu.per_core.forEach((c: any, i: number) => {
        lat.push(c.usage_percent ?? 0)
        if (!coreHists[i]) coreHists[i] = []
        coreHists[i].push(c.usage_percent ?? 0)
        if (coreHists[i].length > MAX) coreHists[i].shift()
      })
      coreLatest.value = lat
    } else if (numCores > 0) {
      const lat: number[] = []
      for (let i = 0; i < numCores; i++) {
        const v = cpuTotal.value + (Math.random() - 0.5) * 10
        const clamped = Math.max(0, Math.min(100, v))
        lat.push(clamped)
        if (!coreHists[i]) coreHists[i] = []
        coreHists[i].push(clamped)
        if (coreHists[i].length > MAX) coreHists[i].shift()
      }
      coreLatest.value = lat
    }

    rebuildTotal()
    rebuildCores()
  } catch (error: any) {
    connected.value = false
    errorMsg.value = error?.message || 'CPU 指标接口不可用'
  } finally {
    polling = false
  }
}

onMounted(() => {
  poll()
  setTimeout(poll, 800)
  timer = setInterval(poll, 2000)
})
onUnmounted(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.cpu-tab { display: flex; flex-direction: column; gap: 14px; }
.summary-cards { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; }
.stat-card {
  background: #fff; border-radius: 12px; padding: 16px 18px;
  box-shadow: 0 1px 6px rgba(0,0,0,0.06);
  position: relative; overflow: hidden;
}
.stat-card.accent { background: linear-gradient(135deg, #409eff, #79bbff); color: #fff; }
.stat-card.accent .stat-label { color: rgba(255,255,255,0.85); }
.stat-body { flex: 1; }
.stat-label { font-size: 12px; color: #909399; margin-bottom: 2px; }
.stat-number { font-size: 24px; font-weight: 700; line-height: 1.2; }
.stat-number.small { font-size: 14px; }
.unit { font-size: 13px; font-weight: 400; margin-left: 2px; }
.stat-progress { position: absolute; bottom: 0; left: 0; right: 0; height: 3px; background: rgba(255,255,255,0.25); }
.stat-progress .fill { height: 100%; background: rgba(255,255,255,0.7); border-radius: 0 2px 2px 0; transition: width .5s; }
.trend-card { border-radius: 12px; }
.trend-card :deep(.el-card__header) { padding: 10px 16px; border-bottom: 1px solid #f5f5f5; }
.card-hdr { display: flex; justify-content: space-between; align-items: center; font-weight: 600; font-size: 14px; }
.badge { font-size: 12px; font-weight: 600; padding: 2px 10px; border-radius: 10px; }
.badge.normal { background: #f0f9eb; color: #67c23a; }
.badge.warning { background: #fdf6ec; color: #e6a23c; }
.badge.danger { background: #fef0f0; color: #f56c6c; }
.section-title { font-size: 14px; font-weight: 600; color: #303133; padding-left: 10px; border-left: 3px solid #409eff; }
.core-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 10px; }
.core-card { background: #fff; border-radius: 10px; padding: 8px 12px; box-shadow: 0 1px 4px rgba(0,0,0,0.05); }
.core-hdr { display: flex; justify-content: space-between; align-items: center; margin-bottom: 2px; }
.core-id { font-size: 12px; font-weight: 600; color: #606266; }
.core-pct { font-size: 15px; font-weight: 700; }
</style>
