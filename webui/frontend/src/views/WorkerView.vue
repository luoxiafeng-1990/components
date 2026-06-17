<template>
  <div class="page-container">
    <div class="page-header">
      <h2>Worker 管理</h2>
      <div class="header-actions">
        <el-dropdown trigger="click" @command="handleBatchCommand">
          <el-button>
            批量操作 <el-icon class="el-icon--right"><ArrowDown /></el-icon>
          </el-button>
          <template #dropdown>
            <el-dropdown-menu>
              <el-dropdown-item command="startAll" :icon="VideoPlay">启动全部</el-dropdown-item>
              <el-dropdown-item command="stopAll" :icon="VideoPause">停止全部</el-dropdown-item>
              <el-dropdown-item command="deleteAll" :icon="Delete" divided>删除全部</el-dropdown-item>
            </el-dropdown-menu>
          </template>
        </el-dropdown>
        <el-button type="primary" @click="showCreateDialog">
          <el-icon><Plus /></el-icon> 创建 Worker
        </el-button>
      </div>
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
              <el-button v-if="canEdit(w.state)"
                size="small" @click="showEditDialog(w)" :icon="Edit">
                编辑
              </el-button>
              <el-popover trigger="click" :width="220">
                <template #reference>
                  <el-button size="small" :icon="CopyDocument">复制</el-button>
                </template>
                <div style="display:flex;align-items:center;gap:8px">
                  <span style="white-space:nowrap;font-size:13px">数量</span>
                  <el-input-number v-model="duplicateCount" :min="1" :max="50" size="small" style="width:120px" />
                  <el-button type="primary" size="small" @click="handleDuplicate(w, duplicateCount)">确定</el-button>
                </div>
              </el-popover>
              <el-button v-if="canEdit(w.state)"
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
              <el-tooltip v-for="c in (w.consumers_config || [])" :key="c.id"
                :content="formatConsumerConfig(c)" placement="top" :show-after="300">
                <el-tag class="consumer-tag"
                  :type="consumerTagType(c.type)" closable @close="handleRemoveConsumer(w.id, c.id)">
                  {{ c.type }}
                </el-tag>
              </el-tooltip>
              <!-- 旧版兼容：如果没有 consumers_config 则显示 consumers -->
              <template v-if="!w.consumers_config || w.consumers_config.length === 0">
                <el-tag v-for="c in w.consumers" :key="c" class="consumer-tag"
                  :type="consumerTagType(c)" closable @close="handleRemoveConsumerByType(w.id, c)">
                  {{ c }}
                </el-tag>
              </template>
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
        <el-form-item label="循环播放">
          <el-switch v-model="createForm.loop" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="createDialogVisible = false">取消</el-button>
        <el-button type="primary" @click="handleCreate">创建</el-button>
      </template>
    </el-dialog>

    <!-- 添加消费者对话框 -->
    <el-dialog v-model="consumerDialogVisible" title="添加消费者" width="560px">
      <el-form :model="consumerForm" label-width="120px">
        <el-form-item label="消费类型" required>
          <el-select v-model="consumerForm.type" placeholder="选择消费类型" style="width:100%"
            @change="onConsumerTypeChange(consumerForm)">
            <el-option value="DISPLAY" label="HDMI 显示 (DISPLAY)" />
            <el-option value="SAVE_RAW" label="保存原始帧 (SAVE_RAW)" />
            <el-option value="SAVE_ENCODED" label="保存编码流 (SAVE_ENCODED)" />
            <el-option value="COMPARE" label="质量分析 (COMPARE)" />
            <el-option value="OPENCV" label="OpenCV (OPENCV)" />
            <el-option value="NPU_INFERENCE" label="NPU 推理 (NPU_INFERENCE)" />
            <el-option value="JPEG_PREVIEW" label="JPEG 预览 (JPEG_PREVIEW)" />
            <el-option value="COUNT" label="帧计数 (COUNT)" />
          </el-select>
        </el-form-item>

        <!-- 动态渲染所有参数 -->
        <consumer-config-fields :config="consumerForm.config" :type="consumerForm.type" />
      </el-form>
      <template #footer>
        <el-button @click="consumerDialogVisible = false">取消</el-button>
        <el-button type="primary" @click="handleAddConsumer">添加</el-button>
      </template>
    </el-dialog>

    <!-- 编辑 Worker 对话框 -->
    <el-dialog v-model="editDialogVisible" title="编辑 Worker" width="700px" top="5vh">
      <el-scrollbar max-height="70vh">
      <el-form :model="editForm" label-width="120px">
        <el-form-item label="名称">
          <el-input v-model="editForm.name" />
        </el-form-item>
        <el-form-item label="数据源">
          <el-select v-model="editForm.datasource_id" style="width:100%">
            <el-option v-for="ds in dsStore.datasources" :key="ds.id"
              :label="`${ds.name} (${ds.type}: ${ds.path})`" :value="ds.id" />
          </el-select>
        </el-form-item>
        <el-form-item label="Worker 类型">
          <el-select v-model="editForm.worker_type" style="width:100%">
            <el-option value="FFMPEG_DECODE" label="解码 (FFMPEG_DECODE)" />
            <el-option value="FFMPEG_ENCODE" label="编码 (FFMPEG_ENCODE)" />
            <el-option value="FFMPEG_PACKET_RECORDER" label="录包 (FFMPEG_PACKET_RECORDER)" />
          </el-select>
        </el-form-item>

        <el-form-item label="循环播放">
          <el-switch v-model="editForm.loop" />
        </el-form-item>

        <el-divider content-position="left">解码器参数</el-divider>
        <el-form-item label="硬件加速">
          <el-switch v-model="editForm.decoder.enable_hardware" />
        </el-form-item>
        <el-form-item label="解码器名称">
          <el-input v-model="editForm.decoder.name" placeholder="自动检测（如 h264_taco）" />
        </el-form-item>
        <el-form-item label="解码线程数">
          <el-input-number v-model="editForm.decoder.decode_threads" :min="0" :max="16" />
        </el-form-item>

        <el-divider content-position="left">消费者配置</el-divider>
        <div v-for="(c, idx) in editForm.consumers" :key="idx" class="edit-consumer-item">
          <div class="edit-consumer-header">
            <el-select v-model="c.type" size="small" style="width:180px"
              @change="onConsumerTypeChange(c)">
              <el-option value="DISPLAY" label="DISPLAY" />
              <el-option value="SAVE_RAW" label="SAVE_RAW" />
              <el-option value="SAVE_ENCODED" label="SAVE_ENCODED" />
              <el-option value="NPU_INFERENCE" label="NPU_INFERENCE" />
              <el-option value="JPEG_PREVIEW" label="JPEG_PREVIEW" />
              <el-option value="OPENCV" label="OPENCV" />
              <el-option value="COUNT" label="COUNT" />
            </el-select>
            <el-button size="small" type="danger" text @click="editForm.consumers.splice(idx, 1)">移除</el-button>
          </div>
          <consumer-config-fields :config="c.config" :type="c.type" />
        </div>
        <el-button type="primary" text @click="showAddConsumerToEdit" :icon="Plus">添加消费者</el-button>
      </el-form>
      </el-scrollbar>
      <template #footer>
        <el-button @click="editDialogVisible = false">取消</el-button>
        <el-button type="primary" @click="handleUpdate">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onMounted, onUnmounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Plus, Delete, Edit, VideoPlay, VideoPause, CopyDocument, ArrowDown } from '@element-plus/icons-vue'
import { useWorkerStore } from '../stores/worker'
import { useDataSourceStore } from '../stores/datasource'
import { workerApi, consumerApi, type WorkerStatus } from '../api'
import ConsumerConfigFields from '../components/ConsumerConfigFields.vue'

const workerStore = useWorkerStore()
const dsStore = useDataSourceStore()

const createDialogVisible = ref(false)
const consumerDialogVisible = ref(false)
const editDialogVisible = ref(false)
const currentWorkerId = ref('')
const editingWorkerId = ref('')
const workerStatuses = ref<Record<string, WorkerStatus>>({})
const duplicateCount = ref(1)
let statusTimer: ReturnType<typeof setInterval> | null = null

const createForm = reactive({
  name: '',
  datasource_id: '',
  worker_type: 'FFMPEG_DECODE',
  enable_hardware: true,
  loop: true,
})

const consumerForm = reactive({
  type: '',
  config: {} as Record<string, any>,
})

const editForm = reactive({
  name: '',
  datasource_id: '',
  worker_type: 'FFMPEG_DECODE',
  loop: true,
  decoder: { name: '', enable_hardware: true, decode_threads: 0 } as Record<string, any>,
  consumers: [] as { type: string; config: Record<string, any>; configJson?: string }[],
})

const CONSUMER_DEFAULTS: Record<string, Record<string, any>> = {
  DISPLAY: { vendor: 'tacopro', target_fps: 30, osd: false, osd_fps: 1, view_type: '', screen_width: 1920, screen_height: 1080, bpp: 32, frame_width: 1920, frame_height: 1080, slot_assignment: '', main_ratio: 0.75 },
  NPU_INFERENCE: { model_path: '', conf_threshold: 0.25, nms_threshold: 0.45, npu_core: 0, physical_addr: false, draw: false, inference_interval: 1 },
  JPEG_PREVIEW: { encoder_name: 'jpeg_taco', quality: 80, target_fps: 15 },
  SAVE_RAW: { output_path: '', format: 'nv12', frames: 10, decoder: '' },
  SAVE_ENCODED: { output_path: '', format: 'mp4', duration: -1 },
  OPENCV: { case: '', params: '', max_frames: -1, psnr: false, ssim: false, verbose: false },
  COMPARE: { psnr: false, ssim: false, min_psnr: 30, min_ssim: 0.95 },
  COUNT: {},
}

function onConsumerTypeChange(form: { type: string; config: Record<string, any> }) {
  const defaults = CONSUMER_DEFAULTS[form.type] || {}
  form.config = { ...defaults }
}

function canEdit(state: string) {
  return state === 'CREATED' || state === 'STOPPED' || state === 'ERROR'
}

function formatConsumerConfig(c: { type: string; config: Record<string, any> }) {
  const entries = Object.entries(c.config || {})
  if (entries.length === 0) return c.type
  return entries.map(([k, v]) => `${k}: ${v}`).join(', ')
}

function showEditDialog(w: any) {
  editingWorkerId.value = w.id
  editForm.name = w.name
  editForm.datasource_id = w.datasource_id
  editForm.worker_type = w.worker_type
  editForm.loop = w.loop !== undefined ? w.loop : true
  editForm.decoder = { ...(w.decoder || { enable_hardware: true, decode_threads: 0 }) }
  editForm.consumers = (w.consumers_config || []).map((c: any) => ({
    type: c.type,
    config: { ...(c.config || {}) },
    configJson: JSON.stringify(c.config || {}, null, 2),
  }))
  editDialogVisible.value = true
}

function showAddConsumerToEdit() {
  editForm.consumers.push({
    type: 'DISPLAY',
    config: { ...CONSUMER_DEFAULTS['DISPLAY'] },
    configJson: '{}',
  })
}

async function handleUpdate() {
  const consumers = editForm.consumers.map(c => {
    const config = { ...c.config }
    // 清理空值
    for (const [k, v] of Object.entries(config)) {
      if (v === '' || v === null || v === undefined) delete config[k]
    }
    return { type: c.type, config }
  })
  try {
    await workerStore.update(editingWorkerId.value, {
      name: editForm.name,
      datasource_id: editForm.datasource_id,
      worker_type: editForm.worker_type,
      loop: editForm.loop,
      decoder: editForm.decoder,
      consumers,
    })
    ElMessage.success('Worker 配置已更新')
    editDialogVisible.value = false
  } catch (e: any) {
    ElMessage.error(e.message || '更新失败')
  }
}

async function handleRemoveConsumer(workerId: string, consumerId: string) {
  try {
    await workerStore.removeConsumer(workerId, consumerId)
    ElMessage.success('消费者已移除')
  } catch (e: any) {
    ElMessage.error(e.message)
  }
}

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
      loop: createForm.loop,
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

function nextWorkerIndex(): number {
  let max = 0
  for (const w of workerStore.workers) {
    const m = w.name.match(/(\d+)\s*$/)
    if (m) {
      const n = parseInt(m[1], 10)
      if (n > max) max = n
    }
  }
  return max + 1
}

async function handleDuplicate(w: any, count: number) {
  const consumers = (w.consumers_config || []).map((c: any) => ({
    type: c.type,
    config: { ...(c.config || {}) }
  }))
  // 去掉源名称末尾的数字部分，作为基础名
  const baseName = w.name.replace(/\s*\d+\s*$/, '').trim() || 'Worker'
  let seq = nextWorkerIndex()
  let ok = 0, fail = 0
  for (let i = 0; i < count; i++) {
    try {
      await workerApi.create({
        name: `${baseName} ${seq++}`,
        datasource_id: w.datasource_id,
        worker_type: w.worker_type,
        decoder: w.decoder ? { ...w.decoder } : undefined,
        consumers,
      })
      ok++
    } catch {
      fail++
    }
  }
  await workerStore.fetchList()
  if (fail === 0) {
    ElMessage.success(`已复制 ${ok} 个 Worker`)
  } else {
    ElMessage.warning(`成功 ${ok} 个，失败 ${fail} 个`)
  }
  duplicateCount.value = 1
}

async function handleDelete(id: string) {
  try {
    await workerStore.remove(id)
    ElMessage.success('Worker 已删除')
  } catch (e: any) {
    ElMessage.error(e.message)
  }
}

async function handleBatchCommand(cmd: string) {
  const workers = workerStore.workers
  if (workers.length === 0) {
    ElMessage.info('暂无 Worker')
    return
  }

  if (cmd === 'startAll') {
    const targets = workers.filter(w => w.state === 'CREATED' || w.state === 'STOPPED' || w.state === 'ERROR')
    if (targets.length === 0) { ElMessage.info('没有可启动的 Worker'); return }
    const results = await Promise.allSettled(targets.map(w => workerStore.start(w.id)))
    const ok = results.filter(r => r.status === 'fulfilled').length
    ElMessage.success(`已启动 ${ok}/${targets.length} 个 Worker`)
  } else if (cmd === 'stopAll') {
    const targets = workers.filter(w => w.state === 'RUNNING' || w.state === 'STARTING')
    if (targets.length === 0) { ElMessage.info('没有运行中的 Worker'); return }
    try {
      const { workerApi } = await import('../api')
      await (workerApi as any).stopAll()
    } catch {
      await Promise.allSettled(targets.map(w => workerStore.stop(w.id)))
    }
    await workerStore.fetchList()
    ElMessage.success(`已停止 ${targets.length} 个 Worker`)
  } else if (cmd === 'deleteAll') {
    try {
      await ElMessageBox.confirm(
        `确定删除全部 ${workers.length} 个 Worker？此操作不可撤销。`,
        '批量删除', { type: 'warning', confirmButtonText: '全部删除', cancelButtonText: '取消' }
      )
    } catch { return }
    const running = workers.filter(w => w.state === 'RUNNING' || w.state === 'STARTING')
    if (running.length > 0) {
      try {
        const { workerApi } = await import('../api')
        await (workerApi as any).stopAll()
      } catch {
        await Promise.allSettled(running.map(w => workerStore.stop(w.id)))
      }
    }
    const ids = workers.map(w => w.id)
    const results = await Promise.allSettled(ids.map(id => workerStore.remove(id)))
    const ok = results.filter(r => r.status === 'fulfilled').length
    ElMessage.success(`已删除 ${ok} 个 Worker`)
  }
}

function showAddConsumerDialog(workerId: string) {
  currentWorkerId.value = workerId
  consumerForm.type = ''
  consumerForm.config = {}
  consumerDialogVisible.value = true
}

async function handleAddConsumer() {
  if (!consumerForm.type) {
    ElMessage.warning('请选择消费类型')
    return
  }

  // 过滤掉空字符串和默认值，直接发送完整 config
  const config = { ...consumerForm.config }
  for (const [k, v] of Object.entries(config)) {
    if (v === '' || v === null || v === undefined) delete config[k]
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

let polling = false
async function pollStatuses() {
  if (polling) return
  polling = true
  try {
    const running = workerStore.workers.filter(w => w.state === 'RUNNING')
    if (running.length === 0) return

    const results = await Promise.allSettled(
      running.map(w =>
        workerApi.status(w.id)
          .then(res => ({ id: w.id, data: res.data.data }))
      )
    )
    for (const r of results) {
      if (r.status === 'fulfilled') {
        workerStatuses.value[r.value.id] = r.value.data
      }
    }
  } finally {
    polling = false
  }
}

onMounted(async () => {
  await Promise.all([workerStore.fetchList(), dsStore.fetchList()])
  statusTimer = setInterval(pollStatuses, 3000)
})

onUnmounted(() => {
  if (statusTimer) {
    clearInterval(statusTimer)
    statusTimer = null
  }
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

.header-actions {
  display: flex;
  align-items: center;
  gap: 10px;
}

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

.edit-consumer-item {
  border: 1px solid #ebeef5;
  border-radius: 6px;
  padding: 12px;
  margin-bottom: 12px;
  background: #fafafa;
}

.edit-consumer-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 8px;
}

.nested-form-item {
  margin-bottom: 8px;
}

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
