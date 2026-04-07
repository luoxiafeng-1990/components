<template>
  <div class="page-container">
    <div class="page-header">
      <h2>数据源管理</h2>
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
        <el-form-item label="路径/地址" prop="path">
          <div class="path-input">
            <el-input v-model="form.path"
              :placeholder="form.type === 'RTSP' ? 'rtsp://192.168.1.100:554/stream' : '/data/videos/test.mp4'" />
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
    <el-dialog v-model="vlcTipVisible" title="RTSP 预览" width="400px">
      <div class="vlc-tip">
        <el-icon :size="48" color="#409eff"><VideoCamera /></el-icon>
        <p>RTSP 流将通过 VLC 播放器打开</p>
        <p class="vlc-hint">请确保已安装 <a href="https://www.videolan.org/" target="_blank">VLC 播放器</a></p>
        <p class="vlc-url">{{ previewRtspUrl }}</p>
      </div>
      <template #footer>
        <el-button @click="vlcTipVisible = false">取消</el-button>
        <el-button type="primary" @click="openVlcStream">打开 VLC</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, reactive } from 'vue'
import { ElMessage, type FormInstance, type FormRules } from 'element-plus'
import { Plus, Edit, Delete, VideoPlay, FolderOpened, CircleCheck, CircleClose, VideoCamera } from '@element-plus/icons-vue'
import { useDataSourceStore } from '../stores/datasource'
import { datasourceApi, type DataSource } from '../api'
import FileBrowser from '../components/FileBrowser.vue'

const store = useDataSourceStore()

const dialogVisible = ref(false)
const isEdit = ref(false)
const editId = ref('')
const formRef = ref<FormInstance>()
const showFileBrowser = ref(false)
const vlcTipVisible = ref(false)
const previewRtspUrl = ref('')

const form = reactive({
  name: '',
  type: 'FILE' as 'FILE' | 'RTSP' | 'BUFFER',
  path: '',
  buffer_count: 0,
  max_frames: -1,
  loop: false,
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
  Object.assign(form, { name: '', type: 'FILE', path: '', buffer_count: 0, max_frames: -1, loop: false })
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

async function handleSubmit() {
  try {
    await formRef.value?.validate()
    if (isEdit.value) {
      await store.update(editId.value, { ...form })
      ElMessage.success('数据源更新成功')
    } else {
      await store.add({ ...form })
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
    vlcTipVisible.value = true
  } else if (row.type === 'FILE') {
    const url = datasourceApi.previewUrl(row.id)
    window.open(url, '_blank')
  } else {
    ElMessage.warning('BUFFER 类型不支持直接预览')
  }
}

function openVlcStream() {
  const url = datasourceApi.previewUrl(
    store.datasources.find(d => d.path === previewRtspUrl.value)?.id || ''
  )
  const link = document.createElement('a')
  link.href = url
  link.download = 'stream.m3u'
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

.vlc-tip {
  text-align: center;
  padding: 20px;
}

.vlc-tip p {
  margin: 12px 0;
  color: #606266;
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
</style>
