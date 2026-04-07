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
          @error="onImgError" alt="preview" />
      </div>
      <el-empty v-else description="请选择要预览的 Worker" />
    </div>

    <!-- 多路宫格预览 -->
    <div v-else class="preview-grid" :class="`grid-${layout}`">
      <div v-for="(w, i) in gridWorkers" :key="i" class="preview-cell">
        <template v-if="w">
          <div class="cell-header">
            <span>{{ w.name }}</span>
            <el-tag size="small" type="success">LIVE</el-tag>
          </div>
          <img :src="streamUrl(w.id)" class="preview-img"
            @error="onImgError" alt="preview" />
        </template>
        <div v-else class="cell-empty">
          <el-icon :size="32" color="#ddd"><VideoCamera /></el-icon>
          <span>空闲</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Refresh, VideoCamera } from '@element-plus/icons-vue'
import { useWorkerStore } from '../stores/worker'
import { previewApi, type Worker } from '../api'

const workerStore = useWorkerStore()

const layout = ref('3x3')
const selectedWorker = ref('')

const previewableWorkers = computed(() =>
  workerStore.workers.filter(w =>
    w.consumers.includes('JPEG_PREVIEW') &&
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

function streamUrl(workerId: string) {
  return previewApi.streamUrl(workerId) + '?t=' + Date.now()
}

function getWorkerName(id: string) {
  return workerStore.workers.find(w => w.id === id)?.name || id
}

function onLayoutChange() {
  selectedWorker.value = ''
}

function refreshStreams() {
  workerStore.fetchList()
}

function onImgError(e: Event) {
  const img = e.target as HTMLImageElement
  img.style.display = 'none'
}

onMounted(() => {
  workerStore.fetchList()
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
</style>
