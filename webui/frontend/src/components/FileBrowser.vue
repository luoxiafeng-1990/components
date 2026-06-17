<template>
  <div class="file-browser">
    <div class="browser-header">
      <el-breadcrumb separator="/">
        <el-breadcrumb-item
          v-for="(seg, i) in pathSegments"
          :key="i"
          @click="navigateTo(seg.path)"
          class="clickable"
        >
          {{ seg.name || '/' }}
        </el-breadcrumb-item>
      </el-breadcrumb>
    </div>

    <div class="browser-toolbar">
      <el-input v-model="manualPath" placeholder="输入路径..." size="small" @keyup.enter="navigateTo(manualPath)">
        <template #append>
          <el-button @click="navigateTo(manualPath)">前往</el-button>
        </template>
      </el-input>
    </div>

    <el-table :data="entries" v-loading="loading" size="small" @row-dblclick="handleDblClick"
      highlight-current-row class="browser-table" max-height="400">
      <el-table-column label="名称" min-width="200">
        <template #default="{ row }">
          <div class="file-name">
            <el-icon v-if="row.type === 'directory'" color="#e6a23c"><Folder /></el-icon>
            <el-icon v-else color="#409eff"><Document /></el-icon>
            <span>{{ row.name }}</span>
          </div>
        </template>
      </el-table-column>
      <el-table-column label="大小" width="100" align="right">
        <template #default="{ row }">
          {{ row.type === 'directory' ? '-' : formatSize(row.size_bytes) }}
        </template>
      </el-table-column>
      <el-table-column label="类型" width="80" align="center">
        <template #default="{ row }">
          <span>{{ row.type === 'directory' ? '目录' : row.extension }}</span>
        </template>
      </el-table-column>
    </el-table>

    <div class="browser-footer" v-if="selectedFile">
      <span class="selected-path">{{ selectedFile }}</span>
      <el-button type="primary" size="small" @click="$emit('select', selectedFile)">选择</el-button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { filesystemApi, type FileEntry } from '../api'
import { Folder, Document } from '@element-plus/icons-vue'

const emit = defineEmits<{ select: [path: string] }>()

const currentPath = ref('/')
const manualPath = ref('/')
const entries = ref<FileEntry[]>([])
const loading = ref(false)
const selectedFile = ref('')

const pathSegments = computed(() => {
  const parts = currentPath.value.split('/').filter(Boolean)
  const segs = [{ name: '/', path: '/' }]
  let accum = ''
  for (const p of parts) {
    accum += '/' + p
    segs.push({ name: p, path: accum + '/' })
  }
  return segs
})

async function navigateTo(path: string) {
  loading.value = true
  selectedFile.value = ''
  try {
    const res = await filesystemApi.browse(path, 'video')
    currentPath.value = res.data.data.current_path
    manualPath.value = currentPath.value
    entries.value = res.data.data.entries
  } catch {
    entries.value = []
  } finally {
    loading.value = false
  }
}

function handleDblClick(row: FileEntry) {
  if (row.type === 'directory') {
    navigateTo(row.path)
  } else {
    selectedFile.value = row.path
    emit('select', row.path)
  }
}

function formatSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' B'
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB'
  if (bytes < 1024 * 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + ' MB'
  return (bytes / 1024 / 1024 / 1024).toFixed(2) + ' GB'
}

onMounted(() => navigateTo('/'))
</script>

<style scoped>
.file-browser {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.browser-header {
  padding: 4px 0;
}

.clickable {
  cursor: pointer;
}

.file-name {
  display: flex;
  align-items: center;
  gap: 6px;
}

.browser-footer {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 8px 0;
  border-top: 1px solid #ebeef5;
}

.selected-path {
  font-family: monospace;
  font-size: 13px;
  color: #606266;
  overflow: hidden;
  text-overflow: ellipsis;
}
</style>
