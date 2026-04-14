<template>
  <div class="npu-tab">
    <el-row :gutter="16">
      <el-col :span="8">
        <el-card shadow="never" class="card">
          <template #header><div class="card-hdr"><span>NPU 使用率</span></div></template>
          <v-chart v-if="hasData" :option="gaugeOpt" autoresize style="height: 220px;" />
          <el-empty v-else description="等待数据..." :image-size="60" />
        </el-card>
      </el-col>
      <el-col :span="16">
        <el-card shadow="never" class="card">
          <template #header><div class="card-hdr"><span>NPU 使用率趋势</span></div></template>
          <v-chart v-if="hasData" :option="trendOpt" autoresize style="height: 220px;" />
          <el-empty v-else description="等待数据..." :image-size="60" />
        </el-card>
      </el-col>
    </el-row>

    <el-row :gutter="16">
      <el-col :span="12">
        <el-card shadow="never" class="card">
          <template #header><div class="card-hdr"><span>编解码性能</span></div></template>
          <div class="codec-stats">
            <div class="codec-item">
              <div class="codec-label">解码 FPS</div>
              <div class="codec-value">{{ decFps.toFixed(1) }}</div>
            </div>
            <div class="codec-item">
              <div class="codec-label">编码 FPS</div>
              <div class="codec-value">{{ encFps.toFixed(1) }}</div>
            </div>
          </div>
          <v-chart v-if="hasData" :option="codecOpt" autoresize style="height: 160px;" />
        </el-card>
      </el-col>
      <el-col :span="12">
        <el-card shadow="never" class="card">
          <template #header>
            <div class="card-hdr">
              <span>NPU 状态</span>
              <el-tag :type="npuAvailable ? 'success' : 'info'" size="small">{{ npuAvailable ? '在线' : '未检测' }}</el-tag>
            </div>
          </template>
          <div class="info-items">
            <div class="info-row"><span class="info-key">使用率</span><span class="info-val">{{ npuPct.toFixed(1) }}%</span></div>
            <div class="info-row"><span class="info-key">解码 FPS</span><span class="info-val">{{ decFps.toFixed(1) }}</span></div>
            <div class="info-row"><span class="info-key">编码 FPS</span><span class="info-val">{{ encFps.toFixed(1) }}</span></div>
            <div class="info-row"><span class="info-key">连接</span>
              <el-tag :type="connected ? 'success' : 'danger'" size="small">{{ connected ? '正常' : '断开' }}</el-tag>
            </div>
          </div>
        </el-card>
      </el-col>
    </el-row>

    <el-card v-if="smiRaw" shadow="never" class="card">
      <template #header><div class="card-hdr"><span>tps-smi 输出</span></div></template>
      <pre class="terminal">{{ smiRaw }}</pre>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { GaugeChart, LineChart } from 'echarts/charts'
import { GridComponent, TooltipComponent, LegendComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import { systemApi } from '../../api'

use([GaugeChart, LineChart, CanvasRenderer, GridComponent, TooltipComponent, LegendComponent])

const MAX = 60
const connected = ref(false)
const hasData = ref(false)
const npuPct = ref(0)
const npuAvailable = ref(false)
const smiRaw = ref('')
const decFps = ref(0)
const encFps = ref(0)
const gaugeOpt = ref<Record<string, any>>({})
const trendOpt = ref<Record<string, any>>({})
const codecOpt = ref<Record<string, any>>({})

const labels: string[] = []
const npuH: number[] = []
const decH: number[] = []
const encH: number[] = []
let timer: ReturnType<typeof setInterval> | null = null

function rebuild() {
  gaugeOpt.value = {
    series: [{
      type: 'gauge', startAngle: 220, endAngle: -40, min: 0, max: 100,
      progress: { show: true, width: 16, roundCap: true, itemStyle: { color: '#e6a23c' } },
      axisLine: { lineStyle: { width: 16, color: [[1, '#f0f0f0']] } },
      axisTick: { show: false }, splitLine: { show: false }, axisLabel: { show: false },
      pointer: { show: false },
      detail: { valueAnimation: true, formatter: '{value}%', fontSize: 28, fontWeight: 'bold',
        offsetCenter: [0, '10%'], color: '#e6a23c' },
      title: { fontSize: 13, color: '#909399', offsetCenter: [0, '55%'] },
      data: [{ value: Math.round(npuPct.value * 10) / 10, name: 'NPU' }]
    }]
  }
  trendOpt.value = {
    tooltip: { trigger: 'axis' },
    grid: { left: 42, right: 12, top: 10, bottom: 22 },
    xAxis: { type: 'category', data: [...labels], boundaryGap: false, show: false },
    yAxis: { type: 'value', min: 0, max: 100,
      axisLabel: { formatter: '{value}%', fontSize: 11, color: '#aaa' },
      splitLine: { lineStyle: { color: '#f5f5f5' } } },
    series: [{ type: 'line', data: [...npuH], smooth: true, symbol: 'none',
      lineStyle: { width: 2, color: '#e6a23c' },
      areaStyle: { color: { type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
        colorStops: [{ offset: 0, color: 'rgba(230,162,60,0.3)' }, { offset: 1, color: 'rgba(230,162,60,0.02)' }] } }
    }]
  }
  codecOpt.value = {
    tooltip: { trigger: 'axis' },
    legend: { data: ['解码', '编码'], top: 0, textStyle: { fontSize: 11 } },
    grid: { left: 40, right: 12, top: 26, bottom: 20 },
    xAxis: { type: 'category', data: [...labels], boundaryGap: false, show: false },
    yAxis: { type: 'value', min: 0, splitLine: { lineStyle: { color: '#f5f5f5' } },
      axisLabel: { fontSize: 11, color: '#aaa' } },
    series: [
      { name: '解码', type: 'line', data: [...decH], smooth: true, symbol: 'none', lineStyle: { width: 1.5, color: '#409eff' } },
      { name: '编码', type: 'line', data: [...encH], smooth: true, symbol: 'none', lineStyle: { width: 1.5, color: '#e6a23c' } },
    ]
  }
}

async function poll() {
  try {
    const m = (await systemApi.metrics()).data.data
    connected.value = true
    hasData.value = true
    const t = new Date(m.timestamp).toLocaleTimeString('zh-CN', { hour12: false })
    labels.push(t); if (labels.length > MAX) labels.shift()
    npuPct.value = m.npu?.usage_percent ?? 0
    npuAvailable.value = m.npu?.available ?? false
    smiRaw.value = m.npu?.raw_output ?? ''
    decFps.value = m.codec?.decode?.fps ?? 0
    encFps.value = m.codec?.encode?.fps ?? 0
    npuH.push(npuPct.value); if (npuH.length > MAX) npuH.shift()
    decH.push(decFps.value); if (decH.length > MAX) decH.shift()
    encH.push(encFps.value); if (encH.length > MAX) encH.shift()
    rebuild()
  } catch { connected.value = false }
}

onMounted(() => { poll(); setTimeout(poll, 800); timer = setInterval(poll, 2000) })
onUnmounted(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.npu-tab { display: flex; flex-direction: column; gap: 14px; }
.card { border-radius: 12px; }
.card :deep(.el-card__header) { padding: 10px 16px; border-bottom: 1px solid #f5f5f5; }
.card-hdr { display: flex; justify-content: space-between; align-items: center; font-weight: 600; font-size: 14px; }
.codec-stats { display: flex; gap: 24px; margin-bottom: 8px; padding: 0 8px; }
.codec-item { flex: 1; }
.codec-label { font-size: 12px; color: #909399; }
.codec-value { font-size: 22px; font-weight: 700; color: #303133; }
.info-items { display: flex; flex-direction: column; gap: 10px; }
.info-row { display: flex; justify-content: space-between; align-items: center; padding: 6px 0; border-bottom: 1px solid #f5f5f5; }
.info-key { font-size: 13px; color: #909399; }
.info-val { font-size: 14px; font-weight: 600; color: #303133; }
.terminal {
  background: #1a1a2e; color: #e0e0e0; padding: 12px 14px; border-radius: 8px;
  font-family: 'JetBrains Mono','Courier New',monospace; font-size: 12px;
  line-height: 1.6; max-height: 280px; overflow: auto; white-space: pre; margin: 0;
}
</style>
