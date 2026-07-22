<template>
  <div class="page-container">
    <div class="page-header">
      <h2>实时预览</h2>
      <div class="header-controls">
        <el-tag type="info" size="small" effect="plain">
          {{ layoutSlots.length }} 路 · {{ gridCols }}×{{ gridRows }}
        </el-tag>
        <el-tag v-if="singleWorkerId" type="warning" size="small" effect="plain">
          单路 · {{ singleWorkerId }}
        </el-tag>

        <el-button size="small" @click="refreshStreams" :icon="Refresh">刷新</el-button>

        <span style="margin-left:12px;font-size:13px;color:#606266">帧率:</span>
        <el-select v-model="previewFps" size="small" style="width:80px" @change="onFpsChange">
          <el-option :value="5" label="5" />
          <el-option :value="10" label="10" />
          <el-option :value="15" label="15" />
          <el-option :value="25" label="25" />
          <el-option :value="30" label="30" />
        </el-select>
      </div>
    </div>

    <!-- 预览区域 -->
    <div class="preview-grid-container">
      <el-empty v-if="layoutSlots.length === 0 && !singleStreamUrl" description="没有运行中的 DISPLAY Worker / 布局为空" />

      <!-- 单路放大模式 -->
      <div v-else-if="singleStreamUrl" class="preview-composite">
        <div class="cell-header">
          <span>单路预览 · {{ singleWorkerName || singleWorkerId }}</span>
          <el-tag size="small" type="success">SESSION</el-tag>
        </div>
        <div class="composite-wrapper">
          <img
            :src="singleStreamUrl"
            class="preview-img composite-preview"
            alt="single preview"
            @dblclick="exitSinglePreview"
            @error="onImgError"
            @load="onImgLoad"
          />
          <div class="single-hint">双击退出单路预览</div>
        </div>
      </div>

      <!-- Composite 合成预览 -->
      <div v-else class="preview-composite">
        <div class="cell-header">
          <span>合成预览 ({{ layoutSlots.length }}路 · {{ gridCols }}×{{ gridRows }})</span>
          <el-tag v-if="compositeAvailable" size="small" type="warning">COMPOSITE</el-tag>
          <el-tag v-else size="small" type="info">等待合成...</el-tag>
        </div>
        <div class="composite-wrapper">
          <img v-if="compositeAvailable" :src="compositeStreamUrl"
            class="preview-img composite-preview"
            @error="onImgError" @load="onImgLoad" alt="composite preview" />
          <div v-else class="composite-loading">
            <el-icon :size="48" class="is-loading"><Loading /></el-icon>
            <p>正在等待合成画面就绪...</p>
          </div>
          <!-- Clickable slot overlay from GET /api/preview/layout -->
          <div class="grid-overlay" v-if="compositeAvailable && layout.width > 0">
            <div v-for="col in Math.max(0, gridCols - 1)" :key="'vc' + col"
              class="grid-line-v"
              :style="{ left: (col / gridCols * 100) + '%' }" />
            <div v-for="row in Math.max(0, gridRows - 1)" :key="'hr' + row"
              class="grid-line-h"
              :style="{ top: (row / gridRows * 100) + '%' }" />
            <div
              v-for="slot in layoutSlots"
              :key="'slot' + slot.slot + '-' + slot.worker_id"
              class="grid-slot-cell"
              :style="slotStyle(slot)"
              @dblclick="onSlotDblClick(slot)"
            >
              <div class="grid-channel-label">
                {{ slot.worker_name || slot.worker_id || ('slot ' + slot.slot) }}
                <span v-if="slot.worker_id && channelFps[slot.worker_id] !== undefined" class="fps-badge">
                  {{ channelFps[slot.worker_id] }} fps
                </span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onBeforeUnmount, reactive } from 'vue'
import { Refresh, Loading } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import { previewApi, type PreviewLayout, type PreviewLayoutSlot } from '../api'
import axios from 'axios'

const previewFps = ref(25)
const compositeAvailable = ref(false)
const compositeStreamUrl = ref('')
const channelFps = reactive<Record<string, number>>({})

const layout = ref<PreviewLayout>({
  width: 0,
  height: 0,
  rows: 0,
  cols: 0,
  view_type: 'grid',
  slots: [],
})

const activeSessionId = ref<string | null>(null)
const singleWorkerId = ref<string | null>(null)
const singleStreamUrl = ref('')
const sessionBusy = ref(false)

const layoutSlots = computed(() =>
  (layout.value.slots || []).filter(s => !!s.worker_id)
)

const gridCols = computed(() => Math.max(1, layout.value.cols || 1))
const gridRows = computed(() => Math.max(1, layout.value.rows || 1))

const singleWorkerName = computed(() => {
  if (!singleWorkerId.value) return ''
  const s = layoutSlots.value.find(x => x.worker_id === singleWorkerId.value)
  return s?.worker_name || ''
})

function slotStyle(slot: PreviewLayoutSlot) {
  const w = layout.value.width || 1
  const h = layout.value.height || 1
  return {
    left: (slot.x / w * 100) + '%',
    top: (slot.y / h * 100) + '%',
    width: (slot.width / w * 100) + '%',
    height: (slot.height / h * 100) + '%',
  }
}

async function onFpsChange(fps: number) {
  try {
    await previewApi.setCompositeConfig({ target_fps: fps })
  } catch {
    try {
      await axios.post('/api/preview/fps', { fps })
    } catch { /* ignore */ }
  }
}

async function fetchLayout() {
  try {
    const res = await previewApi.layout()
    if (res.data?.data) {
      layout.value = res.data.data
    }
  } catch {
    layout.value = {
      width: 0, height: 0, rows: 0, cols: 0, view_type: 'grid', slots: [],
    }
  }
}

async function exitSinglePreview() {
  if (!activeSessionId.value) {
    singleWorkerId.value = null
    singleStreamUrl.value = ''
    return
  }
  const sid = activeSessionId.value
  try {
    await previewApi.deleteSession(sid)
  } catch { /* ignore */ }
  activeSessionId.value = null
  singleWorkerId.value = null
  singleStreamUrl.value = ''
}

async function onSlotDblClick(slot: PreviewLayoutSlot) {
  if (sessionBusy.value) return
  if (!slot.worker_id) return

  if (activeSessionId.value) {
    sessionBusy.value = true
    try {
      await exitSinglePreview()
    } finally {
      sessionBusy.value = false
    }
    return
  }

  sessionBusy.value = true
  try {
    const defaults = { fps: 15, quality: 80, encoder: 'jpeg_taco' }
    const res = await previewApi.createSession({
      worker_id: slot.worker_id,
      fps: defaults.fps,
      quality: defaults.quality,
      encoder: defaults.encoder,
    })
    const data = res.data.data
    // START succeeded — only then switch UI
    activeSessionId.value = data.session_id
    singleWorkerId.value = data.worker_id
    const url = data.stream_url || ''
    singleStreamUrl.value = url.includes('?')
      ? url + '&t=' + Date.now()
      : url + '?t=' + Date.now()
  } catch (e: any) {
    // Keep Composite; show explicit error
    const msg =
      e?.response?.data?.message ||
      e?.message ||
      '启动单路预览失败'
    ElMessage.error(msg)
  } finally {
    sessionBusy.value = false
  }
}

let compositeRetryTimer: ReturnType<typeof setInterval> | null = null
let layoutTimer: ReturnType<typeof setInterval> | null = null

function stopCompositeRetry() {
  if (compositeRetryTimer) {
    clearInterval(compositeRetryTimer)
    compositeRetryTimer = null
  }
}

function startCompositePolling() {
  stopCompositeRetry()
  checkCompositeAvailability()
  compositeRetryTimer = setInterval(async () => {
    if (compositeAvailable.value) return
    await checkCompositeAvailability()
  }, 1000)
}

watch(layoutSlots, () => {
  if (layoutSlots.value.length > 0 && !compositeAvailable.value) {
    startCompositePolling()
  }
})

function refreshStreams() {
  fetchLayout()
  checkCompositeAvailability()
}

async function checkCompositeAvailability() {
  try {
    const res = await axios.get('/api/preview/composite/available', { timeout: 2000 })
    compositeAvailable.value = !!(res.data?.data?.available)
  } catch {
    compositeAvailable.value = false
  }
  if (compositeAvailable.value) {
    compositeStreamUrl.value = '/api/preview/composite/stream?t=' + Date.now()
  }
}

function onImgError(_e: Event) {
  // 预加载模式：加载失败时保留旧帧，不做处理
}

function onImgLoad(_e: Event) {
  // 预加载模式：新帧加载完成后才替换 src，无需额外处理
}

let fpsTimer: ReturnType<typeof setInterval> | null = null

async function fetchChannelFps() {
  try {
    const res = await axios.get('/api/preview/channel-fps')
    if (res.data?.data) {
      const data = res.data.data
      for (const key of Object.keys(channelFps)) {
        if (!(key in data)) delete channelFps[key]
      }
      for (const [k, v] of Object.entries(data)) {
        channelFps[k] = v as number
      }
    }
  } catch { /* ignore */ }
}

function startFpsPolling() {
  stopFpsPolling()
  fetchChannelFps()
  fpsTimer = setInterval(fetchChannelFps, 2000)
}

function stopFpsPolling() {
  if (fpsTimer) {
    clearInterval(fpsTimer)
    fpsTimer = null
  }
}

function startLayoutPolling() {
  stopLayoutPolling()
  fetchLayout()
  layoutTimer = setInterval(fetchLayout, 3000)
}

function stopLayoutPolling() {
  if (layoutTimer) {
    clearInterval(layoutTimer)
    layoutTimer = null
  }
}

function disconnectAllStreams() {
  stopCompositeRetry()
  stopFpsPolling()
  stopLayoutPolling()
}

onMounted(async () => {
  try {
    const res = await previewApi.compositeConfig()
    if (res.data?.data?.target_fps) previewFps.value = res.data.data.target_fps
  } catch {
    try {
      const res = await axios.get('/api/preview/fps')
      if (res.data?.data?.fps) previewFps.value = res.data.data.fps
    } catch { /* ignore */ }
  }
  await fetchLayout()
  await checkCompositeAvailability()
  if (!compositeAvailable.value) {
    startCompositePolling()
  }
  startFpsPolling()
  startLayoutPolling()
})

onBeforeUnmount(async () => {
  if (activeSessionId.value) {
    try { await previewApi.deleteSession(activeSessionId.value) } catch { /* ignore */ }
    activeSessionId.value = null
    singleWorkerId.value = null
    singleStreamUrl.value = ''
  }
  disconnectAllStreams()
})
</script>

<style scoped>
.page-container { padding: 20px; }

.page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 20px;
  flex-wrap: wrap;
  gap: 12px;
}

.page-header h2 { font-size: 20px; color: #303133; }

.header-controls {
  display: flex;
  align-items: center;
  gap: 12px;
}

.cell-header {
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 6px 10px;
  background: linear-gradient(180deg, rgba(0,0,0,0.7) 0%, transparent 100%);
  color: #fff;
  font-size: 13px;
  z-index: 2;
  pointer-events: none;
}

.preview-img {
  width: 100%;
  height: 100%;
  object-fit: contain;
}

.preview-grid-container {
  width: 100%;
}

.preview-composite {
  background: #1a1a1a;
  border-radius: 6px;
  overflow: hidden;
  position: relative;
  max-width: 100%;
}

.composite-preview {
  width: 100%;
  height: auto;
  display: block;
}

.composite-wrapper {
  position: relative;
  width: 100%;
}

.composite-loading {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  min-height: 400px;
  color: #909399;
  gap: 16px;
}

.composite-loading p {
  font-size: 14px;
}

.grid-overlay {
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  /* Allow dblclick on cells; lines stay non-interactive */
  pointer-events: none;
}

.grid-line-v {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  background: rgba(255, 255, 255, 0.35);
  box-shadow: 0 0 4px rgba(0, 0, 0, 0.5);
  pointer-events: none;
}

.grid-line-h {
  position: absolute;
  left: 0;
  right: 0;
  height: 2px;
  background: rgba(255, 255, 255, 0.35);
  box-shadow: 0 0 4px rgba(0, 0, 0, 0.5);
  pointer-events: none;
}

.grid-slot-cell {
  position: absolute;
  box-sizing: border-box;
  pointer-events: auto;
  cursor: pointer;
}

.grid-slot-cell:hover {
  background: rgba(64, 158, 255, 0.12);
  outline: 1px solid rgba(64, 158, 255, 0.45);
}

.grid-channel-label {
  position: absolute;
  left: 0;
  top: 0;
  padding: 4px 8px;
  color: #fff;
  font-size: 12px;
  background: rgba(0, 0, 0, 0.5);
  border-radius: 0 0 4px 0;
  white-space: nowrap;
  max-width: 90%;
  overflow: hidden;
  text-overflow: ellipsis;
  pointer-events: none;
}

.fps-badge {
  display: inline-block;
  margin-left: 6px;
  padding: 1px 5px;
  background: rgba(64, 158, 255, 0.75);
  border-radius: 3px;
  font-size: 11px;
  font-weight: 500;
}

.single-hint {
  position: absolute;
  bottom: 12px;
  left: 50%;
  transform: translateX(-50%);
  padding: 4px 10px;
  background: rgba(0, 0, 0, 0.55);
  color: #fff;
  font-size: 12px;
  border-radius: 4px;
  pointer-events: none;
}
</style>
