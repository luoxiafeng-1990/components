<template>
  <div class="page-container">
    <div class="page-header">
      <h2>实时预览</h2>
      <div class="header-controls">
        <el-radio-group v-model="layout" @change="onLayoutChange" size="small">
          <el-radio-button value="1x1">1路</el-radio-button>
          <el-radio-button value="2x2">2×2</el-radio-button>
          <el-radio-button value="3x3">3×3</el-radio-button>
          <el-radio-button value="4x4">4×4</el-radio-button>
        </el-radio-group>

        <el-select v-if="layout === '1x1'" v-model="selectedWorker"
          placeholder="选择 Worker" size="small" style="width: 200px">
          <el-option v-for="w in previewableWorkers" :key="w.id"
            :label="w.name" :value="w.id" />
        </el-select>

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

    <!-- 单路预览 -->
    <div v-if="layout === '1x1'" class="preview-single">
      <div v-if="selectedWorker" class="preview-cell large">
        <div class="cell-header">
          <span>{{ getWorkerName(selectedWorker) }}</span>
          <el-tag size="small" type="success">LIVE</el-tag>
        </div>
        <img :src="streamUrl(selectedWorker)" class="preview-img"
          @error="onImgError" @load="onImgLoad" alt="preview" />
      </div>
      <el-empty v-else description="请选择要预览的 Worker" />
    </div>

    <!-- 多路宫格预览 -->
    <div v-else class="preview-grid-container">
      <!-- Composite stream (single stitched image for all channels) -->
      <div v-if="compositeAvailable" class="preview-composite">
        <div class="cell-header">
          <span>合成预览 ({{ layout }})</span>
          <el-tag size="small" type="warning">COMPOSITE</el-tag>
        </div>
        <img :src="compositeStreamUrl" class="preview-img composite-preview"
          @error="onImgError" @load="onImgLoad" alt="composite preview" />
      </div>

      <!-- Fallback: per-worker snapshot polling -->
      <div v-else class="preview-grid" :class="`grid-${layout}`">
        <div v-for="(w, i) in gridWorkers" :key="i" class="preview-cell">
          <template v-if="w">
            <div class="cell-header">
              <span>{{ w.name }}</span>
              <el-tag size="small" type="success">LIVE</el-tag>
            </div>
            <img :src="snapshotSrcs[w.id] || ''" class="preview-img"
              @error="onImgError" @load="onImgLoad" alt="preview" />
          </template>
          <div v-else class="cell-empty">
            <el-icon :size="32" color="#ddd"><VideoCamera /></el-icon>
            <span>空闲</span>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onBeforeUnmount, reactive, type Ref } from 'vue'
import { Refresh, VideoCamera } from '@element-plus/icons-vue'
import { useWorkerStore } from '../stores/worker'
import { previewApi, type Worker } from '../api'
import axios from 'axios'

const workerStore = useWorkerStore()

const layout = ref('3x3')
const selectedWorker = ref('')
const previewFps = ref(15)
const compositeAvailable = ref(false)
const compositeStreamUrl = ref('')

async function onFpsChange(fps: number) {
  try {
    await axios.post('/api/preview/fps', { fps })
  } catch { /* ignore */ }
  // 重启轮询定时器以应用新帧率
  if (layout.value !== '1x1' && snapshotTimer) {
    startSnapshotPolling()
  }
}

const previewableWorkers = computed(() =>
  workerStore.workers.filter(w =>
    (w.consumers?.includes('JPEG_PREVIEW') ||
     w.consumers_config?.some((c: any) => c.type === 'JPEG_PREVIEW')) &&
    (w.state === 'RUNNING' || w.state === 'STARTING')
  )
)

const gridWorkers = computed(() => {
  const dim = parseInt(layout.value[0])
  const total = dim * dim
  const result: (Worker | null)[] = []
  for (let i = 0; i < total; i++) {
    result.push(previewableWorkers.value[i] || null)
  }
  return result
})

// --- snapshot 轮询（宫格模式使用，规避浏览器 6 连接限制） ---
const snapshotSrcs = reactive<Record<string, string>>({})
let snapshotTimer: ReturnType<typeof setInterval> | null = null
let snapshotBatchIdx = 0
const BATCH_SIZE = 4

const snapshotInterval = computed(() => {
  const fps = previewFps.value
  return Math.max(100, Math.round(1000 / fps))
})

function preloadAndSwap(workerId: string) {
  const url = previewApi.snapshotUrl(workerId) + '&t=' + Date.now()
  const img = new Image()
  img.onload = () => { snapshotSrcs[workerId] = url }
  img.onerror = () => { /* keep old frame on error */ }
  img.src = url
}

function refreshSnapshots() {
  const workers = previewableWorkers.value
  if (workers.length === 0) return
  if (workers.length <= BATCH_SIZE) {
    for (const w of workers) {
      preloadAndSwap(w.id)
    }
  } else {
    const start = snapshotBatchIdx % workers.length
    for (let i = 0; i < BATCH_SIZE && i < workers.length; i++) {
      const idx = (start + i) % workers.length
      preloadAndSwap(workers[idx].id)
    }
    snapshotBatchIdx = (start + BATCH_SIZE) % workers.length
  }
}

function startSnapshotPolling() {
  stopSnapshotPolling()
  snapshotBatchIdx = 0
  for (const w of previewableWorkers.value) {
    preloadAndSwap(w.id)
  }
  snapshotTimer = setInterval(refreshSnapshots, snapshotInterval.value)
}

function stopSnapshotPolling() {
  if (snapshotTimer) {
    clearInterval(snapshotTimer)
    snapshotTimer = null
  }
}

// 1x1 用 MJPEG 流，宫格用 snapshot 轮询（或 composite stream）
watch(layout, async (val) => {
  selectedWorker.value = ''
  if (val === '1x1') {
    stopSnapshotPolling()
  } else {
    await checkCompositeAvailability()
    if (!compositeAvailable.value) {
      startSnapshotPolling()
    } else {
      stopSnapshotPolling()
    }
  }
}, { immediate: false })

watch(previewableWorkers, () => {
  if (layout.value !== '1x1') refreshSnapshots()
})

function streamUrl(workerId: string) {
  return previewApi.streamUrl(workerId) + '?t=' + Date.now()
}

function getWorkerName(id: string) {
  return workerStore.workers.find(w => w.id === id)?.name || id
}

function onLayoutChange() {
  // handled by watch(layout)
}

function refreshStreams() {
  workerStore.fetchList()
  if (layout.value !== '1x1') {
    checkCompositeAvailability()
    if (!compositeAvailable.value) refreshSnapshots()
  }
}

async function checkCompositeAvailability() {
  try {
    const res = await axios.get('/api/preview/composite/snapshot', { responseType: 'blob', timeout: 3000 })
    compositeAvailable.value = (res.status === 200)
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

function disconnectAllStreams() {
  stopSnapshotPolling()
  for (const key of Object.keys(snapshotSrcs)) {
    delete snapshotSrcs[key]
  }
  selectedWorker.value = ''
}

onMounted(async () => {
  await workerStore.fetchList()
  try {
    const res = await axios.get('/api/preview/fps')
    if (res.data?.data?.fps) previewFps.value = res.data.data.fps
  } catch { /* ignore */ }
  if (layout.value !== '1x1') {
    await checkCompositeAvailability()
    if (!compositeAvailable.value) {
      startSnapshotPolling()
    }
  }
})

onBeforeUnmount(() => {
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

.preview-single {
  display: flex;
  justify-content: center;
}

.preview-grid {
  display: grid;
  gap: 8px;
}

.grid-2x2 { grid-template-columns: repeat(2, 1fr); }
.grid-3x3 { grid-template-columns: repeat(3, 1fr); }
.grid-4x4 { grid-template-columns: repeat(4, 1fr); }

.preview-cell {
  background: #1a1a1a;
  border-radius: 6px;
  overflow: hidden;
  aspect-ratio: 16 / 9;
  display: flex;
  flex-direction: column;
  position: relative;
}

.preview-cell.large {
  max-width: 960px;
  width: 100%;
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
  z-index: 1;
}

.preview-img {
  width: 100%;
  height: 100%;
  object-fit: contain;
}

.cell-empty {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  color: #666;
  font-size: 12px;
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
</style>
