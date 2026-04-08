<template>
  <div class="page-container">
    <div class="page-header">
      <h2>硬件信息</h2>
      <el-tag :type="connected ? 'success' : 'danger'" size="small">
        {{ connected ? '实时采集中' : '连接中断' }}
      </el-tag>
    </div>

    <!-- 系统基础信息卡片 -->
    <el-row :gutter="16" class="info-cards">
      <el-col :span="6">
        <el-card shadow="hover" class="stat-card">
          <div class="stat-label">TPS 版本</div>
          <div class="stat-value version">{{ sysInfo.tps_version || '--' }}</div>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card shadow="hover" class="stat-card">
          <div class="stat-label">系统内核</div>
          <div class="stat-value small">{{ sysInfo.kernel || '--' }}</div>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card shadow="hover" class="stat-card">
          <div class="stat-label">CPU</div>
          <div class="stat-value small">{{ sysInfo.cpu_model || sysInfo.arch || '--' }}</div>
          <div class="stat-sub">{{ sysInfo.cpu_cores }} 核</div>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card shadow="hover" class="stat-card">
          <div class="stat-label">运行时间</div>
          <div class="stat-value">{{ formatUptime(sysInfo.uptime_seconds ?? 0) }}</div>
          <div class="stat-sub">{{ sysInfo.hostname }}</div>
        </el-card>
      </el-col>
    </el-row>

    <!-- 实时仪表 -->
    <el-row :gutter="16" class="gauge-row">
      <el-col :span="8">
        <el-card shadow="hover">
          <div class="chart-title">CPU 使用率</div>
          <v-chart :option="cpuGaugeOpt" autoresize class="gauge-chart" />
        </el-card>
      </el-col>
      <el-col :span="8">
        <el-card shadow="hover">
          <div class="chart-title">内存使用率</div>
          <v-chart :option="memGaugeOpt" autoresize class="gauge-chart" />
        </el-card>
      </el-col>
      <el-col :span="8">
        <el-card shadow="hover">
          <div class="chart-title">NPU 使用率</div>
          <v-chart :option="npuGaugeOpt" autoresize class="gauge-chart" />
        </el-card>
      </el-col>
    </el-row>

    <!-- 趋势曲线图 -->
    <el-row :gutter="16" class="chart-row">
      <el-col :span="12">
        <el-card shadow="hover">
          <div class="chart-title">CPU / 内存 / NPU 趋势</div>
          <v-chart :option="usageTrendOpt" autoresize class="trend-chart" />
        </el-card>
      </el-col>
      <el-col :span="12">
        <el-card shadow="hover">
          <div class="chart-title">网络吞吐量</div>
          <v-chart :option="networkTrendOpt" autoresize class="trend-chart" />
        </el-card>
      </el-col>
    </el-row>

    <el-row :gutter="16" class="chart-row">
      <el-col :span="12">
        <el-card shadow="hover">
          <div class="chart-title">内存详情</div>
          <v-chart :option="memDetailOpt" autoresize class="trend-chart" />
        </el-card>
      </el-col>
      <el-col :span="12">
        <el-card shadow="hover">
          <div class="chart-title">编解码性能</div>
          <v-chart :option="codecTrendOpt" autoresize class="trend-chart" />
        </el-card>
      </el-col>
    </el-row>

    <!-- 网卡详情表格 -->
    <el-card shadow="hover" class="net-table-card">
      <div class="chart-title">网卡信息</div>
      <el-table :data="latestNetwork" size="small" stripe>
        <el-table-column prop="name" label="接口" width="100" />
        <el-table-column prop="ip" label="IP 地址" width="180" />
        <el-table-column label="接收速率">
          <template #default="{ row }">{{ row.rx_rate_kbps.toFixed(1) }} KB/s</template>
        </el-table-column>
        <el-table-column label="发送速率">
          <template #default="{ row }">{{ row.tx_rate_kbps.toFixed(1) }} KB/s</template>
        </el-table-column>
        <el-table-column label="总接收">
          <template #default="{ row }">{{ formatBytes(row.rx_bytes) }}</template>
        </el-table-column>
        <el-table-column label="总发送">
          <template #default="{ row }">{{ formatBytes(row.tx_bytes) }}</template>
        </el-table-column>
        <el-table-column label="错误">
          <template #default="{ row }">
            <el-text :type="(row.rx_errors + row.tx_errors) > 0 ? 'danger' : 'info'">
              RX: {{ row.rx_errors }} / TX: {{ row.tx_errors }}
            </el-text>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- NPU 原始输出 -->
    <el-card v-if="latestNpuRaw" shadow="hover" class="net-table-card">
      <div class="chart-title">tps-smi 输出</div>
      <pre class="raw-output">{{ latestNpuRaw }}</pre>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, shallowRef } from 'vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { GaugeChart, LineChart } from 'echarts/charts'
import {
  TitleComponent, TooltipComponent, LegendComponent,
  GridComponent, DataZoomComponent
} from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import { systemApi, type SystemInfo, type SystemMetrics, type NetworkInterface } from '../api'

use([
  GaugeChart, LineChart, CanvasRenderer,
  TitleComponent, TooltipComponent, LegendComponent,
  GridComponent, DataZoomComponent
])

const MAX_POINTS = 120

const sysInfo = ref<Partial<SystemInfo>>({})
const connected = ref(true)
let timer: ReturnType<typeof setInterval> | null = null

// 时序数据存储
const timeLabels = shallowRef<string[]>([])
const cpuHistory = shallowRef<number[]>([])
const memHistory = shallowRef<number[]>([])
const npuHistory = shallowRef<number[]>([])
const netRxHistory = shallowRef<number[]>([])
const netTxHistory = shallowRef<number[]>([])
const memUsedHistory = shallowRef<number[]>([])
const memCachedHistory = shallowRef<number[]>([])
const memBuffersHistory = shallowRef<number[]>([])
const decFpsHistory = shallowRef<number[]>([])
const encFpsHistory = shallowRef<number[]>([])

const latestCpu = ref(0)
const latestMem = ref(0)
const latestNpu = ref(0)
const latestMemInfo = ref({ total_mb: 0, used_mb: 0 })
const latestNetwork = ref<NetworkInterface[]>([])
const latestNpuRaw = ref('')

function pushData(arr: number[], val: number): number[] {
  const next = [...arr, val]
  if (next.length > MAX_POINTS) next.shift()
  return next
}

function pushLabel(arr: string[], val: string): string[] {
  const next = [...arr, val]
  if (next.length > MAX_POINTS) next.shift()
  return next
}

async function fetchInfo() {
  try {
    const res = await systemApi.info()
    sysInfo.value = res.data.data
  } catch { /* ignore */ }
}

async function fetchMetrics() {
  try {
    const res = await systemApi.metrics()
    const m: SystemMetrics = res.data.data
    connected.value = true

    const t = new Date(m.timestamp).toLocaleTimeString('zh-CN', { hour12: false })
    timeLabels.value = pushLabel(timeLabels.value, t)

    latestCpu.value = m.cpu.usage_percent
    latestMem.value = m.memory.usage_percent
    latestNpu.value = m.npu.usage_percent
    latestMemInfo.value = { total_mb: m.memory.total_mb, used_mb: m.memory.used_mb }
    latestNetwork.value = m.network
    latestNpuRaw.value = m.npu.raw_output

    cpuHistory.value = pushData(cpuHistory.value, m.cpu.usage_percent)
    memHistory.value = pushData(memHistory.value, m.memory.usage_percent)
    npuHistory.value = pushData(npuHistory.value, m.npu.usage_percent)

    // 网络：所有接口速率加总
    const totalRx = m.network.reduce((s, n) => s + n.rx_rate_kbps, 0)
    const totalTx = m.network.reduce((s, n) => s + n.tx_rate_kbps, 0)
    netRxHistory.value = pushData(netRxHistory.value, totalRx)
    netTxHistory.value = pushData(netTxHistory.value, totalTx)

    memUsedHistory.value = pushData(memUsedHistory.value, m.memory.used_mb)
    memCachedHistory.value = pushData(memCachedHistory.value, m.memory.cached_mb)
    memBuffersHistory.value = pushData(memBuffersHistory.value, m.memory.buffers_mb)

    const decFps = m.codec.decode?.fps ?? 0
    const encFps = m.codec.encode?.fps ?? 0
    decFpsHistory.value = pushData(decFpsHistory.value, decFps)
    encFpsHistory.value = pushData(encFpsHistory.value, encFps)
  } catch {
    connected.value = false
  }
}

// ============ Chart Options ============

function makeGaugeOpt(value: number, label: string, color: string) {
  return {
    series: [{
      type: 'gauge',
      startAngle: 210,
      endAngle: -30,
      min: 0,
      max: 100,
      progress: { show: true, width: 14, roundCap: true, itemStyle: { color } },
      axisLine: { lineStyle: { width: 14, color: [[1, '#e6e8eb']] } },
      axisTick: { show: false },
      splitLine: { show: false },
      axisLabel: { show: false },
      pointer: { show: false },
      detail: {
        valueAnimation: true,
        formatter: '{value}%',
        fontSize: 22,
        fontWeight: 'bold',
        offsetCenter: [0, '10%'],
        color
      },
      title: { fontSize: 13, color: '#909399', offsetCenter: [0, '55%'] },
      data: [{ value, name: label }]
    }]
  }
}

const cpuGaugeOpt = computed(() => makeGaugeOpt(latestCpu.value, 'CPU', '#409eff'))
const memGaugeOpt = computed(() =>
  makeGaugeOpt(latestMem.value,
    `${latestMemInfo.value.used_mb}/${latestMemInfo.value.total_mb} MB`,
    '#67c23a'))
const npuGaugeOpt = computed(() => makeGaugeOpt(latestNpu.value, 'NPU', '#e6a23c'))

function makeTrendBase() {
  return {
    tooltip: { trigger: 'axis' as const },
    grid: { left: 50, right: 20, top: 30, bottom: 30 },
    xAxis: { type: 'category' as const, data: timeLabels.value, boundaryGap: false,
      axisLabel: { fontSize: 10, interval: 'auto' as const } },
  }
}

const usageTrendOpt = computed(() => ({
  ...makeTrendBase(),
  legend: { data: ['CPU %', '内存 %', 'NPU %'], top: 0, textStyle: { fontSize: 11 } },
  yAxis: { type: 'value', min: 0, max: 100, axisLabel: { formatter: '{value}%' } },
  series: [
    { name: 'CPU %', type: 'line', data: cpuHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.08 }, itemStyle: { color: '#409eff' } },
    { name: '内存 %', type: 'line', data: memHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.08 }, itemStyle: { color: '#67c23a' } },
    { name: 'NPU %', type: 'line', data: npuHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.08 }, itemStyle: { color: '#e6a23c' } },
  ]
}))

const networkTrendOpt = computed(() => ({
  ...makeTrendBase(),
  legend: { data: ['接收 KB/s', '发送 KB/s'], top: 0, textStyle: { fontSize: 11 } },
  yAxis: { type: 'value', min: 0, axisLabel: { formatter: '{value}' } },
  series: [
    { name: '接收 KB/s', type: 'line', data: netRxHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.15 }, itemStyle: { color: '#409eff' } },
    { name: '发送 KB/s', type: 'line', data: netTxHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.15 }, itemStyle: { color: '#f56c6c' } },
  ]
}))

const memDetailOpt = computed(() => ({
  ...makeTrendBase(),
  legend: { data: ['已用 MB', 'Cached MB', 'Buffers MB'], top: 0, textStyle: { fontSize: 11 } },
  yAxis: { type: 'value', min: 0, axisLabel: { formatter: '{value}' } },
  series: [
    { name: '已用 MB', type: 'line', data: memUsedHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.15 }, itemStyle: { color: '#67c23a' } },
    { name: 'Cached MB', type: 'line', data: memCachedHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.1 }, itemStyle: { color: '#909399' } },
    { name: 'Buffers MB', type: 'line', data: memBuffersHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.1 }, itemStyle: { color: '#e6a23c' } },
  ]
}))

const codecTrendOpt = computed(() => ({
  ...makeTrendBase(),
  legend: { data: ['解码 FPS', '编码 FPS'], top: 0, textStyle: { fontSize: 11 } },
  yAxis: { type: 'value', min: 0, axisLabel: { formatter: '{value}' } },
  series: [
    { name: '解码 FPS', type: 'line', data: decFpsHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.15 }, itemStyle: { color: '#409eff' } },
    { name: '编码 FPS', type: 'line', data: encFpsHistory.value, smooth: true,
      lineStyle: { width: 2 }, symbol: 'none', areaStyle: { opacity: 0.15 }, itemStyle: { color: '#e6a23c' } },
  ]
}))

// ============ Helpers ============

function formatUptime(seconds: number): string {
  if (!seconds) return '--'
  const d = Math.floor(seconds / 86400)
  const h = Math.floor((seconds % 86400) / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  if (d > 0) return `${d}天 ${h}时 ${m}分`
  if (h > 0) return `${h}时 ${m}分`
  return `${m}分`
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return bytes + ' B'
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB'
  if (bytes < 1024 * 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + ' MB'
  return (bytes / 1024 / 1024 / 1024).toFixed(2) + ' GB'
}

onMounted(async () => {
  await fetchInfo()
  await fetchMetrics()
  // 首次 CPU 采样只建立基线，不准确，立即再采一次
  setTimeout(fetchMetrics, 500)
  timer = setInterval(fetchMetrics, 2000)
})

onUnmounted(() => {
  if (timer) clearInterval(timer)
})
</script>

<style scoped>
.page-container { padding: 20px; }

.page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 16px;
}
.page-header h2 { font-size: 20px; color: #303133; }

.info-cards { margin-bottom: 16px; }

.stat-card {
  text-align: center;
  min-height: 100px;
  display: flex;
  flex-direction: column;
  justify-content: center;
}
.stat-card :deep(.el-card__body) {
  padding: 16px;
}
.stat-label {
  font-size: 12px;
  color: #909399;
  margin-bottom: 6px;
  text-transform: uppercase;
  letter-spacing: 1px;
}
.stat-value {
  font-size: 20px;
  font-weight: 700;
  color: #303133;
  word-break: break-all;
}
.stat-value.version { color: #409eff; }
.stat-value.small { font-size: 14px; font-weight: 600; }
.stat-sub {
  font-size: 11px;
  color: #c0c4cc;
  margin-top: 4px;
}

.gauge-row { margin-bottom: 16px; }
.gauge-chart { height: 200px; }
.chart-title {
  font-size: 14px;
  font-weight: 600;
  color: #606266;
  margin-bottom: 8px;
}

.chart-row { margin-bottom: 16px; }
.trend-chart { height: 260px; }

.net-table-card { margin-bottom: 16px; }

.raw-output {
  background: #1e1e1e;
  color: #d4d4d4;
  padding: 12px;
  border-radius: 6px;
  font-family: 'Courier New', monospace;
  font-size: 12px;
  line-height: 1.5;
  max-height: 300px;
  overflow-y: auto;
  white-space: pre-wrap;
  word-break: break-all;
  margin: 0;
}
</style>
