<template>
  <div class="page-container">
    <div class="page-header">
      <h2>Worker 管理</h2>
      <el-button type="primary" @click="showCreateDialog">
        <el-icon><Plus /></el-icon> 创建 Worker
      </el-button>
    </div>

    <div class="worker-list" v-loading="workerStore.loading">
      <el-empty v-if="workerStore.workers.length === 0" description="暂无 Worker，点击上方按钮创建" />

      <el-card v-for="w in workerStore.workers" :key="w.id" class="worker-card" shadow="hover">
        <template #header>
          <div class="card-header">
            <div class="worker-title">
              <span class="worker-name">{{ w.name }}</span>
              <el-tag :type="stateTagType(w.state)" size="small">{{ w.state }}</el-tag>
              <el-tag type="info" size="small">{{ w.worker_type }}</el-tag>
            </div>
            <div class="worker-actions">
              <el-button v-if="w.state === 'CREATED' || w.state === 'STOPPED'"
                size="small" type="success" @click="handleStart(w.id)" :icon="VideoPlay">
                启动
              </el-button>
              <el-button v-if="w.state === 'RUNNING' || w.state === 'STARTING'"
                size="small" type="warning" @click="handleStop(w.id)" :icon="VideoPause">
                停止
              </el-button>
              <el-popconfirm title="确定删除此 Worker？" @confirm="handleDelete(w.id)">
                <template #reference>
                  <el-button size="small" type="danger" :icon="Delete">删除</el-button>
                </template>
              </el-popconfirm>
            </div>
          </div>
        </template>

        <div class="worker-body">
          <div class="worker-info">
            <div class="info-item">
              <span class="info-label">数据源:</span>
              <span>{{ w.datasource_name }} ({{ w.datasource_id }})</span>
            </div>
            <div class="info-item">
              <span class="info-label">解码器:</span>
              <span>{{ w.decoder?.name || '自动' }} | 硬件加速: {{ w.decoder?.enable_hardware ? '是' : '否' }}</span>
            </div>
            <div class="info-item" v-if="workerStatuses[w.id]">
              <span class="info-label">统计:</span>
              <span>
                FPS: {{ workerStatuses[w.id].fps.toFixed(1) }} |
                已解码: {{ workerStatuses[w.id].decoded_frames }} 帧 |
                运行: {{ workerStatuses[w.id].uptime_seconds.toFixed(0) }}s
              </span>
            </div>
            <div class="info-item" v-if="workerStatuses[w.id]?.command_line">
              <span class="info-label">命令:</span>
              <code class="command-line">{{ workerStatuses[w.id].command_line }}</code>
            </div>
          </div>

          <!-- 进程输出日志 -->
          <template v-if="workerStatuses[w.id]?.output">
            <el-divider content-position="left">进程输出</el-divider>
            <pre class="process-output">{{ workerStatuses[w.id].output }}</pre>
          </template>

          <el-divider content-position="left">消费者</el-divider>
          <div class="consumer-section">
            <div class="consumer-tags">
              <el-tag v-for="c in w.consumers" :key="c" class="consumer-tag"
                :type="consumerTagType(c)" closable @close="handleRemoveConsumerByType(w.id, c)">
                {{ c }}
              </el-tag>
              <el-button size="small" @click="showAddConsumerDialog(w.id)" :icon="Plus" circle />
            </div>
          </div>
        </div>
      </el-card>
    </div>

    <!-- 创建 Worker 对话框 -->
    <el-dialog v-model="createDialogVisible" title="创建 Worker" width="500px">
      <el-form :model="createForm" label-width="100px">
        <el-form-item label="名称" required>
          <el-input v-model="createForm.name" placeholder="如：Worker-1" />
        </el-form-item>
        <el-form-item label="数据源" required>
          <el-select v-model="createForm.datasource_id" placeholder="选择数据源" style="width:100%">
            <el-option v-for="ds in dsStore.datasources" :key="ds.id"
              :label="`${ds.name} (${ds.type}: ${ds.path})`" :value="ds.id" />
          </el-select>
        </el-form-item>
        <el-form-item label="Worker 类型">
          <el-select v-model="createForm.worker_type" style="width:100%">
            <el-option value="FFMPEG_DECODE" label="解码 (FFMPEG_DECODE)" />
            <el-option value="FFMPEG_ENCODE" label="编码 (FFMPEG_ENCODE)" />
            <el-option value="FFMPEG_PACKET_RECORDER" label="录包 (FFMPEG_PACKET_RECORDER)" />
          </el-select>
        </el-form-item>
        <el-form-item label="硬件加速">
          <el-switch v-model="createForm.enable_hardware" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="createDialogVisible = false">取消</el-button>
        <el-button type="primary" @click="handleCreate">创建</el-button>
      </template>
    </el-dialog>

    <!-- 添加消费者对话框 -->
    <el-dialog v-model="consumerDialogVisible" title="添加消费者" width="500px">
      <el-form :model="consumerForm" label-width="100px">
        <el-form-item label="消费类型" required>
          <el-select v-model="consumerForm.type" placeholder="选择消费类型" style="width:100%">
            <el-option value="DISPLAY" label="HDMI 显示 (DISPLAY)" />
            <el-option value="SAVE_RAW" label="保存原始数据 (SAVE_RAW)" />
            <el-option value="SAVE_ENCODED" label="保存编码流 (SAVE_ENCODED)" />
            <el-option value="COMPARE" label="质量分析 (COMPARE)" />
            <el-option value="OPENCV" label="OpenCV (OPENCV)" />
            <el-option value="NPU_INFERENCE" label="NPU 推理 (NPU_INFERENCE)" />
            <el-option value="JPEG_PREVIEW" label="JPEG 预览 (JPEG_PREVIEW)" />
            <el-option value="COUNT" label="帧计数 (COUNT)" />
          </el-select>
        </el-form-item>

        <!-- DISPLAY 配置 -->
        <template v-if="consumerForm.type === 'DISPLAY'">
          <el-form-item label="显示厂商">
            <el-select v-model="consumerForm.config.vendor" style="width:100%">
              <el-option value="tacopro" label="TacoPro (默认)" />
              <el-option value="taco" label="Taco" />
            </el-select>
          </el-form-item>
          <el-form-item label="目标帧率">
            <el-input-number v-model="consumerForm.config.target_fps" :min="1" :max="120" />
          </el-form-item>
          <el-form-item label="OSD 叠加">
            <el-switch v-model="consumerForm.config.osd" />
          </el-form-item>
          <el-form-item label="视图类型">
            <el-select v-model="consumerForm.config.view_type" style="width:100%">
              <el-option value="grid" label="网格 (grid)" />
              <el-option value="main_sidebar" label="主画面+侧栏 (main_sidebar)" />
            </el-select>
          </el-form-item>
        </template>

        <!-- SAVE_ENCODED 配置 -->
        <template v-if="consumerForm.type === 'SAVE_ENCODED'">
          <el-form-item label="输出路径">
            <el-input v-model="consumerForm.config.output_path" placeholder="/data/output/record.mp4" />
          </el-form-item>
          <el-form-item label="格式">
            <el-select v-model="consumerForm.config.format" style="width:100%">
              <el-option value="mp4" label="MP4" />
              <el-option value="mkv" label="MKV" />
              <el-option value="ts" label="TS" />
              <el-option value="flv" label="FLV" />
            </el-select>
          </el-form-item>
          <el-form-item label="录制时长(秒)">
            <el-input-number v-model="consumerForm.config.duration" :min="-1" placeholder="-1=无限制" />
          </el-form-item>
        </template>

        <!-- NPU 配置 -->
        <template v-if="consumerForm.type === 'NPU_INFERENCE'">
          <el-form-item label="模型路径" required>
            <el-input v-model="consumerForm.config.model_path" placeholder="/opt/models/yolov5.nb" />
          </el-form-item>
          <el-form-item label="置信度阈值">
            <el-slider v-model="consumerForm.config.conf_threshold" :min="0" :max="1" :step="0.05" show-input />
          </el-form-item>
          <el-form-item label="NMS 阈值">
            <el-slider v-model="consumerForm.config.nms_threshold" :min="0" :max="1" :step="0.05" show-input />
          </el-form-item>
          <el-form-item label="绘制检测框">
            <el-switch v-model="consumerForm.config.draw" />
          </el-form-item>
        </template>

        <!-- JPEG_PREVIEW 配置 -->
        <template v-if="consumerForm.type === 'JPEG_PREVIEW'">
          <el-form-item label="编码器">
            <el-select v-model="consumerForm.config.encoder_name" style="width:100%">
              <el-option value="jpeg_taco" label="jpeg_taco (硬件)" />
              <el-option value="mjpeg" label="mjpeg (软件)" />
            </el-select>
          </el-form-item>
          <el-form-item label="JPEG 质量">
            <el-slider v-model="consumerForm.config.quality" :min="1" :max="100" show-input />
          </el-form-item>
          <el-form-item label="目标帧率">
            <el-input-number v-model="consumerForm.config.target_fps" :min="1" :max="60" />
          </el-form-item>
        </template>

        <!-- SAVE_RAW 配置 -->
        <template v-if="consumerForm.type === 'SAVE_RAW'">
          <el-form-item label="输出路径">
            <el-input v-model="consumerForm.config.output_path_0" placeholder="/data/output/frame" />
          </el-form-item>
          <el-form-item label="像素格式">
            <el-select v-model="consumerForm.config.format" style="width:100%">
              <el-option value="nv12" label="NV12" />
              <el-option value="rgb888" label="RGB888" />
              <el-option value="nv21" label="NV21" />
            </el-select>
          </el-form-item>
          <el-form-item label="保存帧数">
            <el-input-number v-model="consumerForm.config.frames" :min="1" :max="10000" />
          </el-form-item>
        </template>
      </el-form>
      <template #footer>
        <el-button @click="consumerDialogVisible = false">取消</el-button>
        <el-button type="primary" @click="handleAddConsumer">添加</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { Plus, Delete, VideoPlay, VideoPause } from '@element-plus/icons-vue'
import { useWorkerStore } from '../stores/worker'
import { useDataSourceStore } from '../stores/datasource'
import { workerApi, consumerApi, type WorkerStatus } from '../api'

const workerStore = useWorkerStore()
const dsStore = useDataSourceStore()

const createDialogVisible = ref(false)
const consumerDialogVisible = ref(false)
const currentWorkerId = ref('')
const workerStatuses = ref<Record<string, WorkerStatus>>({})
let statusTimer: ReturnType<typeof setInterval> | null = null

const createForm = reactive({
  name: '',
  datasource_id: '',
  worker_type: 'FFMPEG_DECODE',
  enable_hardware: true,
})

const consumerForm = reactive({
  type: '',
  config: {
    device_id: 0,
    output_path: '',
    output_path_0: '',
    format: 'h264',
    model_path: '',
    threshold: 0.5,
    encoder_name: 'jpeg_taco',
    quality: 80,
    target_fps: 15,
  } as Record<string, any>,
})

function stateTagType(state: string) {
  switch (state) {
    case 'RUNNING': return 'success'
    case 'STARTING': return 'warning'
    case 'STOPPING': return 'warning'
    case 'STOPPED': return 'info'
    case 'ERROR': return 'danger'
    default: return ''
  }
}

function consumerTagType(type: string) {
  switch (type) {
    case 'DISPLAY': return 'success'
    case 'JPEG_PREVIEW': return 'primary'
    case 'NPU_INFERENCE': return 'warning'
    case 'SAVE_RAW': case 'SAVE_ENCODED': return 'info'
    default: return ''
  }
}

function showCreateDialog() {
  Object.assign(createForm, { name: '', datasource_id: '', worker_type: 'FFMPEG_DECODE', enable_hardware: true })
  createDialogVisible.value = true
}

async function handleCreate() {
  if (!createForm.name || !createForm.datasource_id) {
    ElMessage.warning('请填写名称并选择数据源')
    return
  }
  try {
    await workerStore.create({
      name: createForm.name,
      datasource_id: createForm.datasource_id,
      worker_type: createForm.worker_type,
    })
    ElMessage.success('Worker 创建成功')
    createDialogVisible.value = false
  } catch (e: any) {
    ElMessage.error(e.message || '创建失败')
  }
}

async function handleStart(id: string) {
  try {
    await workerStore.start(id)
    ElMessage.success('Worker 启动中')
  } catch (e: any) {
    ElMessage.error(e.message)
  }
}

async function handleStop(id: string) {
  try {
    await workerStore.stop(id)
    ElMessage.success('Worker 停止中')
  } catch (e: any) {
    ElMessage.error(e.message)
  }
}

async function handleDelete(id: string) {
  try {
    await workerStore.remove(id)
    ElMessage.success('Worker 已删除')
  } catch (e: any) {
    ElMessage.error(e.message)
  }
}

function showAddConsumerDialog(workerId: string) {
  currentWorkerId.value = workerId
  consumerForm.type = ''
  consumerForm.config = {
    vendor: 'tacopro', target_fps: 30, osd: false, view_type: 'grid',
    output_path: '', output_path_0: '', format: 'h264',
    model_path: '', conf_threshold: 0.25, nms_threshold: 0.45, draw: false,
    encoder_name: 'jpeg_taco', quality: 80,
    duration: -1, frames: 10,
  }
  consumerDialogVisible.value = true
}

async function handleAddConsumer() {
  if (!consumerForm.type) {
    ElMessage.warning('请选择消费类型')
    return
  }

  const config: Record<string, any> = {}
  switch (consumerForm.type) {
    case 'DISPLAY':
      config.vendor = consumerForm.config.vendor
      config.target_fps = consumerForm.config.target_fps
      if (consumerForm.config.osd) config.osd = true
      if (consumerForm.config.view_type !== 'grid') config.view_type = consumerForm.config.view_type
      break
    case 'SAVE_RAW':
      if (consumerForm.config.output_path_0) config.output_path = consumerForm.config.output_path_0
      config.format = consumerForm.config.format
      config.frames = consumerForm.config.frames
      break
    case 'SAVE_ENCODED':
      if (consumerForm.config.output_path) config.output_path = consumerForm.config.output_path
      config.format = consumerForm.config.format
      if (consumerForm.config.duration > 0) config.duration = consumerForm.config.duration
      break
    case 'NPU_INFERENCE':
      config.model_path = consumerForm.config.model_path
      config.conf_threshold = consumerForm.config.conf_threshold
      config.nms_threshold = consumerForm.config.nms_threshold
      if (consumerForm.config.draw) config.draw = true
      break
    case 'JPEG_PREVIEW':
      config.encoder_name = consumerForm.config.encoder_name
      config.quality = consumerForm.config.quality
      config.target_fps = consumerForm.config.target_fps
      break
  }

  try {
    await workerStore.addConsumer(currentWorkerId.value, { type: consumerForm.type, config })
    ElMessage.success('消费者添加成功')
    consumerDialogVisible.value = false
  } catch (e: any) {
    ElMessage.error(e.message || '添加失败')
  }
}

async function handleRemoveConsumerByType(workerId: string, typeName: string) {
  try {
    const res = await consumerApi.list(workerId)
    const consumer = res.data.data.find((c: any) => c.type === typeName)
    if (consumer) {
      await workerStore.removeConsumer(workerId, consumer.id)
      ElMessage.success('消费者已移除')
    }
  } catch (e: any) {
    ElMessage.error(e.message)
  }
}

async function pollStatuses() {
  for (const w of workerStore.workers) {
    if (w.state === 'RUNNING') {
      try {
        const res = await workerApi.status(w.id)
        workerStatuses.value[w.id] = res.data.data
      } catch { /* ignore */ }
    }
  }
}

onMounted(() => {
  workerStore.fetchList()
  dsStore.fetchList()
  statusTimer = setInterval(pollStatuses, 2000)
})

onUnmounted(() => {
  if (statusTimer) clearInterval(statusTimer)
})
</script>

<style scoped>
.page-container { padding: 20px; }

.page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 20px;
}

.page-header h2 { font-size: 20px; color: #303133; }

.worker-list {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.worker-card { border-radius: 8px; }

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.worker-title {
  display: flex;
  align-items: center;
  gap: 8px;
}

.worker-name { font-weight: 600; font-size: 15px; }

.worker-actions { display: flex; gap: 8px; }

.worker-info { display: flex; flex-direction: column; gap: 6px; }

.info-item {
  font-size: 13px;
  color: #606266;
  display: flex;
  gap: 8px;
}

.info-label {
  color: #909399;
  min-width: 60px;
}

.consumer-section { margin-top: 4px; }

.consumer-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
}

.consumer-tag { cursor: default; }

.command-line {
  background: #f5f7fa;
  padding: 2px 8px;
  border-radius: 4px;
  font-family: 'Courier New', monospace;
  font-size: 12px;
  color: #409eff;
  word-break: break-all;
}

.process-output {
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
