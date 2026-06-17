<template>
  <div class="page-container">
    <div class="page-header">
      <h2>添加数据源</h2>
      <div class="header-actions">
        <el-button type="primary" @click="showAddDialog">
          <el-icon><Plus /></el-icon> 添加数据源
        </el-button>
      </div>
    </div>

    <el-table :data="store.datasources" v-loading="store.loading" stripe class="ds-table">
      <el-table-column prop="id" label="ID" width="100" />
      <el-table-column prop="name" label="名称" width="150" />
      <el-table-column prop="type" label="类型" width="100">
        <template #default="{ row }">
          <el-tag :type="typeTagColor(row.type)" size="small">{{ row.type }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="path" label="路径/地址" min-width="250" show-overflow-tooltip />
      <el-table-column prop="buffer_count" label="Buffer数" width="90" align="center" />
      <el-table-column prop="loop" label="循环" width="70" align="center">
        <template #default="{ row }">
          <el-icon v-if="row.loop" color="#67c23a"><CircleCheck /></el-icon>
          <el-icon v-else color="#ccc"><CircleClose /></el-icon>
        </template>
      </el-table-column>
      <el-table-column prop="status" label="状态" width="90" align="center">
        <template #default="{ row }">
          <el-tag :type="row.status === 'in_use' ? 'warning' : 'info'" size="small">
            {{ row.status }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="操作" width="260" fixed="right">
        <template #default="{ row }">
          <el-button size="small" @click="handlePreview(row)" :icon="VideoPlay">
            预览
          </el-button>
          <el-button size="small" @click="handleEdit(row)" :icon="Edit">
            编辑
          </el-button>
          <el-popconfirm title="确定删除此数据源？" @confirm="handleDelete(row.id)">
            <template #reference>
              <el-button size="small" type="danger" :icon="Delete"
                :disabled="row.status === 'in_use'">
                删除
              </el-button>
            </template>
          </el-popconfirm>
        </template>
      </el-table-column>
    </el-table>

    <!-- 添加/编辑对话框 -->
    <el-dialog v-model="dialogVisible" :title="isEdit ? '编辑数据源' : '添加数据源'" width="600px">
      <el-form :model="form" label-width="100px" :rules="formRules" ref="formRef">
        <el-form-item label="名称" prop="name">
          <el-input v-model="form.name" placeholder="如：摄像头1" />
        </el-form-item>
        <el-form-item label="类型" prop="type">
          <el-radio-group v-model="form.type">
            <el-radio-button value="FILE">视频文件</el-radio-button>
            <el-radio-button value="RTSP">RTSP 流</el-radio-button>
            <el-radio-button value="BUFFER">Buffer 模式</el-radio-button>
          </el-radio-group>
        </el-form-item>

        <template v-if="form.type === 'RTSP'">
          <el-alert type="info" :closable="false" show-icon class="rtsp-hint-alert">
            <template #title>
              候选 RTSP 地址请在侧栏「验证 RTSP 数据源」中填写、保存并可选执行「检测可用性」。
            </template>
            <router-link class="rtsp-hint-link" to="/datasources/rtsp-verify">打开验证页面</router-link>
          </el-alert>
          <el-form-item label="路径/地址" prop="path">
            <el-select
              v-model="form.path"
              filterable
              allow-create
              default-first-option
              placeholder="含本机列表与已创建 RTSP 占用的地址"
              class="rtsp-path-select"
            >
              <el-option
                v-for="u in rtspSelectOptions"
                :key="u"
                :label="u"
                :value="u"
              >
                <span class="rtsp-option-row">
                  <span
                    class="rtsp-dot"
                    :class="{
                      ok: rtspDotGreen(u),
                      full: rtspDotRed(u),
                      bad: rtspDotProbeFailed(u),
                      idle: rtspDotUnknown(u),
                    }"
                  />
                  <span class="rtsp-url-text">{{ u }}</span>
                  <span v-if="isUrlUsedByCreatedDatasource(u)" class="rtsp-selected-tag">已选</span>
                </span>
              </el-option>
            </el-select>
          </el-form-item>
        </template>
        <el-form-item v-else label="路径/地址" prop="path">
          <div class="path-input">
            <el-input v-model="form.path"
              :placeholder="form.type === 'BUFFER' ? 'BUFFER 模式路径' : '/data/videos/test.mp4'" />
            <el-button v-if="form.type === 'FILE'" @click="showFileBrowser = true" :icon="FolderOpened">
              浏览
            </el-button>
          </div>
        </el-form-item>
        <el-form-item label="Buffer 数量">
          <el-input-number v-model="form.buffer_count" :min="0" :max="32" />
          <span class="form-hint">0 = 使用默认值</span>
        </el-form-item>
        <el-form-item label="最大帧数">
          <el-input-number v-model="form.max_frames" :min="-1" />
          <span class="form-hint">-1 = 无限制</span>
        </el-form-item>
        <el-form-item label="循环播放">
          <el-switch v-model="form.loop" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="primary" @click="handleSubmit">{{ isEdit ? '保存' : '添加' }}</el-button>
      </template>
    </el-dialog>

    <!-- 文件浏览器对话框 -->
    <el-dialog v-model="showFileBrowser" title="选择视频文件" width="700px">
      <FileBrowser @select="onFileSelected" />
    </el-dialog>

    <!-- VLC 提示 -->
    <el-dialog v-model="vlcTipVisible" title="RTSP 预览" width="480px">
      <div class="vlc-tip vlc-tip--block">
        <div class="vlc-tip-icon">
          <el-icon :size="40" color="#409eff"><VideoCamera /></el-icon>
        </div>
        <p>将下载一个 <code>.m3u</code> 播放列表，用本机 VLC 打开该文件即可拉流（与网页探测一致时，列表内已带 TCP 等选项）。</p>
        <p class="vlc-hint">
          「绿色圆点」表示：运行 Web 的服务器上已对该 RTSP 连续解码约 {{ RTSP_DECODE_PROBE_SECONDS }} 秒无报错。若本机 VLC 仍失败，多为 PC 与摄像机网络与服务器不同。
        </p>
        <p class="vlc-hint">请确保已安装 <a href="https://www.videolan.org/" target="_blank">VLC 播放器</a>。</p>
        <p class="vlc-url">{{ previewRtspUrl }}</p>
        <el-button size="small" @click="copyPreviewRtspUrl">复制 RTSP 地址</el-button>
      </div>
      <template #footer>
        <el-button @click="vlcTipVisible = false">取消</el-button>
        <el-button type="primary" @click="openVlcStream">下载 .m3u</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, reactive, watch, computed } from 'vue'
import { ElMessage, type FormInstance, type FormRules } from 'element-plus'
import { Plus, Edit, Delete, VideoPlay, FolderOpened, CircleCheck, CircleClose, VideoCamera } from '@element-plus/icons-vue'
import { useDataSourceStore } from '../stores/datasource'
import { datasourceApi, type DataSource } from '../api'
import FileBrowser from '../components/FileBrowser.vue'
import {
  RTSP_MAX_CONCURRENT_PER_URL,
  RTSP_DECODE_PROBE_SECONDS,
  normalizeRtspUrl,
  parseRtspLines,
  loadRtspUrlsFromStorage,
  loadRtspProbeCacheFromStorage,
} from '../utils/rtspCandidates'

const store = useDataSourceStore()

const dialogVisible = ref(false)
const isEdit = ref(false)
const editId = ref('')
const formRef = ref<FormInstance>()
const showFileBrowser = ref(false)
const vlcTipVisible = ref(false)
const previewRtspUrl = ref('')
/** 与「打开 VLC」配套，避免仅靠 path 匹配错行 */
const previewRowId = ref('')

/** 与「验证 RTSP 数据源」页共用 localStorage；打开对话框时递增以刷新下拉候选 */
const rtspListRevision = ref(0)
/** url -> 是否在服务器上通过约 10s 解码探测；未探测为 undefined */
const rtspPlayable = ref<Record<string, boolean | undefined>>({})

const form = reactive({
  name: '',
  type: 'FILE' as 'FILE' | 'RTSP' | 'BUFFER',
  path: '',
  buffer_count: 0,
  max_frames: -1,
  loop: false,
})

/**
 * 已有 RTSP 数据源里，有多少条把该 URL 作为 path（每条约占一路连接）。
 * 编辑当前数据源时排除 editId，避免把自己算作「已占用」，便于判断能否继续用原地址。
 */
function occupiedSlotsExcludingEdit(url: string): number {
  const u = normalizeRtspUrl(url)
  const exclude = isEdit.value ? editId.value : ''
  return store.datasources.filter(
    (d) =>
      d.type === 'RTSP' &&
      normalizeRtspUrl(d.path) === u &&
      d.id !== exclude
  ).length
}

/** 至少有一个已创建的数据源将该 URL 作为路径（含正在编辑的这条） */
function isUrlUsedByCreatedDatasource(url: string): boolean {
  const u = normalizeRtspUrl(url)
  return store.datasources.some(
    (d) => d.type === 'RTSP' && normalizeRtspUrl(d.path) === u
  )
}

function probePlayableRaw(url: string): boolean | undefined {
  return rtspPlayable.value[normalizeRtspUrl(url)]
}

/** 绿色：缓存中探测可达，且当前仍有空闲连接槽 */
function rtspDotGreen(url: string): boolean {
  if (probePlayableRaw(url) !== true) return false
  return occupiedSlotsExcludingEdit(url) < RTSP_MAX_CONCURRENT_PER_URL
}

/** 红色：缓存中探测可达，但当前已无空闲槽（不能再拉一路） */
function rtspDotRed(url: string): boolean {
  if (probePlayableRaw(url) !== true) return false
  return occupiedSlotsExcludingEdit(url) >= RTSP_MAX_CONCURRENT_PER_URL
}

/** 灰色：已探测且流不可达 */
function rtspDotProbeFailed(url: string): boolean {
  return probePlayableRaw(url) === false
}

/** 浅灰：尚未探测过（无缓存） */
function rtspDotUnknown(url: string): boolean {
  return probePlayableRaw(url) === undefined
}

/** 按 created_at 取最近创建的一条名称，在其基础上递增数字后缀，避免与已有名称重复 */
function suggestNextDatasourceName(list: DataSource[]): string {
  const names = new Set(list.map((d) => d.name))
  if (list.length === 0) {
    return '数据源1'
  }
  const sorted = [...list].sort(
    (a, b) => new Date(b.created_at).getTime() - new Date(a.created_at).getTime()
  )
  const lastName = sorted[0].name

  const m = lastName.match(/^(.*?)(\d+)$/)
  if (m) {
    const prefix = m[1]
    const digits = m[2]
    const width = digits.length
    let n = parseInt(digits, 10) + 1
    for (let guard = 0; guard < 10000; guard++) {
      const numStr = String(n).padStart(Math.max(width, String(n).length), '0')
      const candidate = prefix + numStr
      if (!names.has(candidate)) return candidate
      n++
    }
  }

  let k = 2
  for (let guard = 0; guard < 10000; guard++) {
    const candidate = `${lastName}${k}`
    if (!names.has(candidate)) return candidate
    k++
  }
  return `${lastName}_${Date.now()}`
}

/**
 * 下拉项 = 「验证 RTSP 数据源」页保存的候选地址 + 已创建 RTSP 数据源的 path
 */
const rtspSelectOptions = computed(() => {
  rtspListRevision.value
  const seen = new Set<string>()
  const out: string[] = []

  function pushUrl(raw: string) {
    const n = normalizeRtspUrl(raw)
    if (!n || seen.has(n)) return
    seen.add(n)
    out.push(n)
  }

  for (const line of parseRtspLines(loadRtspUrlsFromStorage())) {
    pushUrl(line)
  }
  for (const d of store.datasources) {
    if (d.type === 'RTSP' && d.path) {
      pushUrl(d.path)
    }
  }
  if (form.path) {
    pushUrl(form.path)
  }
  return out
})

/** 打开添加/编辑对话框时：刷新候选列表（与验证页 localStorage 同步）与探测结果缓存 */
watch(dialogVisible, (open) => {
  if (!open) return
  rtspListRevision.value++
  rtspPlayable.value = loadRtspProbeCacheFromStorage()
})

const formRules: FormRules = {
  name: [{ required: true, message: '请输入数据源名称', trigger: 'blur' }],
  type: [{ required: true, message: '请选择类型', trigger: 'change' }],
  path: [{ required: true, message: '请输入路径或地址', trigger: 'blur' }],
}

function typeTagColor(type: string) {
  switch (type) {
    case 'FILE': return 'success'
    case 'RTSP': return 'warning'
    case 'BUFFER': return 'info'
    default: return ''
  }
}

function showAddDialog() {
  isEdit.value = false
  editId.value = ''
  const nextName = suggestNextDatasourceName(store.datasources)
  Object.assign(form, {
    name: nextName,
    type: 'FILE',
    path: '',
    buffer_count: 0,
    max_frames: -1,
    loop: false,
  })
  dialogVisible.value = true
}

function handleEdit(row: DataSource) {
  isEdit.value = true
  editId.value = row.id
  Object.assign(form, {
    name: row.name,
    type: row.type,
    path: row.path,
    buffer_count: row.buffer_count,
    max_frames: row.max_frames,
    loop: row.loop,
  })
  dialogVisible.value = true
}

function buildSubmitPayload() {
  const base: Partial<DataSource> = {
    name: form.name,
    type: form.type,
    path: form.path,
    buffer_count: form.buffer_count,
    max_frames: form.max_frames,
    loop: form.loop,
  }
  if (form.type === 'RTSP') {
    base.rtsp_urls = parseRtspLines(loadRtspUrlsFromStorage())
  }
  return base
}

async function handleSubmit() {
  try {
    await formRef.value?.validate()
    const payload = buildSubmitPayload()
    if (isEdit.value) {
      await store.update(editId.value, payload)
      ElMessage.success('数据源更新成功')
    } else {
      await store.add(payload)
      ElMessage.success('数据源添加成功')
    }
    dialogVisible.value = false
  } catch (e: any) {
    ElMessage.error(e.message || '操作失败')
  }
}

async function handleDelete(id: string) {
  try {
    await store.remove(id)
    ElMessage.success('数据源已删除')
  } catch (e: any) {
    ElMessage.error(e.message || '删除失败')
  }
}

function handlePreview(row: DataSource) {
  if (row.type === 'RTSP') {
    previewRtspUrl.value = row.path
    previewRowId.value = row.id
    vlcTipVisible.value = true
  } else if (row.type === 'FILE') {
    const url = datasourceApi.previewUrl(row.id)
    window.open(url, '_blank')
  } else {
    ElMessage.warning('BUFFER 类型不支持直接预览')
  }
}

async function copyPreviewRtspUrl() {
  try {
    await navigator.clipboard.writeText(previewRtspUrl.value)
    ElMessage.success('已复制 RTSP 地址')
  } catch {
    ElMessage.error('复制失败，请手动选中地址复制')
  }
}

function openVlcStream() {
  const id = previewRowId.value
  if (!id) {
    ElMessage.error('无法定位数据源，请从列表中重新点击预览')
    return
  }
  const url = datasourceApi.previewUrl(id)
  const row = store.datasources.find(d => d.id === id)
  const stem = (row?.name || 'stream').replace(/[/\\"<>:*|?]/g, '_').trim() || 'stream'
  const link = document.createElement('a')
  link.href = url
  link.download = `${stem}.m3u`
  link.rel = 'noopener'
  link.click()
  vlcTipVisible.value = false
}

function onFileSelected(path: string) {
  form.path = path
  showFileBrowser.value = false
}

onMounted(() => {
  store.fetchList()
})
</script>

<style scoped>
.page-container {
  padding: 20px;
}

.page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 20px;
}

.page-header h2 {
  font-size: 20px;
  color: #303133;
}

.ds-table {
  width: 100%;
}

.path-input {
  display: flex;
  gap: 8px;
  width: 100%;
}

.path-input .el-input {
  flex: 1;
}

.form-hint {
  margin-left: 12px;
  color: #909399;
  font-size: 12px;
}

.rtsp-hint-alert {
  margin-bottom: 12px;
}

.rtsp-hint-link {
  font-weight: 500;
}

.vlc-tip {
  padding: 8px 0 16px;
}

.vlc-tip--block {
  text-align: left;
}

.vlc-tip-icon {
  text-align: center;
  margin-bottom: 12px;
}

.vlc-tip p {
  margin: 12px 0;
  color: #606266;
  line-height: 1.55;
}

.vlc-hint {
  font-size: 13px;
  color: #909399 !important;
}

.vlc-hint a {
  color: #409eff;
}

.vlc-url {
  font-family: monospace;
  background: #f5f7fa;
  padding: 8px 12px;
  border-radius: 4px;
  word-break: break-all;
}

.rtsp-path-select {
  width: 100%;
}

.rtsp-option-row {
  display: flex;
  align-items: center;
  gap: 8px;
  width: 100%;
  min-width: 0;
}

.rtsp-selected-tag {
  flex-shrink: 0;
  margin-left: auto;
  font-size: 11px;
  line-height: 1.4;
  color: var(--el-color-primary);
  border: 1px solid var(--el-color-primary-light-5);
  border-radius: 4px;
  padding: 1px 8px;
  background: var(--el-color-primary-light-9);
}

.rtsp-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: #c0c4cc;
  flex-shrink: 0;
}

.rtsp-dot.ok {
  background: #67c23a;
}

.rtsp-dot.bad {
  background: #c0c4cc;
}

.rtsp-dot.full {
  background: #f56c6c;
}

.rtsp-dot.idle {
  background: #e4e7ed;
}

.rtsp-url-text {
  flex: 1;
  min-width: 0;
  font-family: ui-monospace, monospace;
  font-size: 12px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
</style>
