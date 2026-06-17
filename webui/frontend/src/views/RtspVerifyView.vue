<template>
  <div class="page-container rtsp-verify-page">
    <div class="page-header">
      <h2>验证 RTSP 数据源</h2>
    </div>

    <el-collapse v-model="activePanels" class="rtsp-collapse">
      <el-collapse-item name="main" title="RTSP 地址列表与可用性检测">
        <div class="collapse-body">
          <p class="lead">
            在此维护候选地址列表；保存后可在「添加数据源」中选择 RTSP
            路径。探测结果保存在本机，下次打开沿用。
          </p>
          <el-input
            v-model="rtspUrlsText"
            type="textarea"
            :autosize="{ minRows: 14, maxRows: 28 }"
            class="rtsp-textarea-large"
            placeholder="每行一个地址，例如：&#10;rtsp://192.168.1.10:554/stream1&#10;rtsp://192.168.1.11:554/live"
          />
          <div class="toolbar">
            <div class="toolbar-left">
              <el-button type="success" :icon="DocumentChecked" @click="handleSave">
                保存
              </el-button>
              <el-button type="primary" plain :loading="rtspProbing" :icon="VideoPlay" @click="runRtspProbe">
                检测可用性
              </el-button>
            </div>
            <span class="form-hint">
              单击「保存」将地址列表写入本机；「检测可用性」在服务器上对每路解码约
              {{ RTSP_DECODE_PROBE_SECONDS }} 秒。绿：验证通过且仍有空闲连接；红：验证通过但并发已满；灰：失败或未测。
              单路并发上限 {{ RTSP_MAX_CONCURRENT_PER_URL }}。
            </span>
          </div>

          <div v-if="parsedUrls.length" class="url-status-list">
            <div class="url-status-head">当前列表状态（保存后于「添加数据源」中生效）</div>
            <div
              v-for="u in parsedUrls"
              :key="u"
              class="url-status-row"
            >
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
            </div>
          </div>
        </div>
      </el-collapse-item>

      <el-collapse-item name="help" title="说明与图例">
        <ul class="help-list">
          <li>绿色：服务器侧已对该地址做约 {{ RTSP_DECODE_PROBE_SECONDS }} 秒解码验证通过，且当前仍有空闲连接。</li>
          <li>红色：验证通过但并发已满，无法再新建一路相同 URL 的数据源。</li>
          <li>灰色：验证失败或未探测。「已选」表示已有数据源占用该路径。</li>
          <li>绿色表示服务器可稳定拉流解码一小段，与您本机 VLC 是否通无关。</li>
        </ul>
      </el-collapse-item>
    </el-collapse>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { ElMessage, ElLoading } from 'element-plus'
import axios from 'axios'
import { DocumentChecked, VideoPlay } from '@element-plus/icons-vue'
import { useDataSourceStore } from '../stores/datasource'
import { datasourceApi } from '../api'
import {
  RTSP_MAX_CONCURRENT_PER_URL,
  RTSP_DECODE_PROBE_SECONDS,
  normalizeRtspUrl,
  parseRtspLines,
  loadRtspUrlsFromStorage,
  saveRtspUrlsToStorage,
  loadRtspProbeCacheFromStorage,
  saveRtspProbeCacheToStorage,
} from '../utils/rtspCandidates'

const store = useDataSourceStore()

const activePanels = ref<string[]>(['main', 'help'])
const rtspUrlsText = ref('')
const rtspProbing = ref(false)
const rtspPlayable = ref<Record<string, boolean | undefined>>({})

const parsedUrls = computed(() => parseRtspLines(rtspUrlsText.value))

function loadAllFromStorage() {
  rtspUrlsText.value = loadRtspUrlsFromStorage()
  rtspPlayable.value = loadRtspProbeCacheFromStorage()
}

function isUrlUsedByCreatedDatasource(url: string): boolean {
  const u = normalizeRtspUrl(url)
  return store.datasources.some(
    (d) => d.type === 'RTSP' && normalizeRtspUrl(d.path) === u
  )
}

function occupiedSlots(url: string): number {
  const u = normalizeRtspUrl(url)
  return store.datasources.filter(
    (d) => d.type === 'RTSP' && normalizeRtspUrl(d.path) === u
  ).length
}

function probePlayableRaw(url: string): boolean | undefined {
  return rtspPlayable.value[normalizeRtspUrl(url)]
}

function rtspDotGreen(url: string): boolean {
  if (probePlayableRaw(url) !== true) return false
  return occupiedSlots(url) < RTSP_MAX_CONCURRENT_PER_URL
}

function rtspDotRed(url: string): boolean {
  if (probePlayableRaw(url) !== true) return false
  return occupiedSlots(url) >= RTSP_MAX_CONCURRENT_PER_URL
}

function rtspDotProbeFailed(url: string): boolean {
  return probePlayableRaw(url) === false
}

function rtspDotUnknown(url: string): boolean {
  return probePlayableRaw(url) === undefined
}

function handleSave() {
  saveRtspUrlsToStorage(rtspUrlsText.value)
  ElMessage.success('已保存到本机，添加数据源时可从列表中选择这些地址')
}

async function runRtspProbe() {
  const urls = parseRtspLines(rtspUrlsText.value)
  if (urls.length === 0) {
    ElMessage.warning('请先填写至少一行 RTSP 地址')
    return
  }
  const maxWallSec = urls.length * 95
  ElMessage.info(
    `将在服务器上对 ${urls.length} 路地址各解码约 ${RTSP_DECODE_PROBE_SECONDS} 秒；全部约 ${maxWallSec} 秒内应结束`
  )
  rtspProbing.value = true
  const loading = ElLoading.service({
    lock: true,
    text: `正在探测 ${urls.length} 路 RTSP（服务器端解码），请稍候…`,
    background: 'rgba(0, 0, 0, 0.35)',
  })
  const nextMap: Record<string, boolean | undefined> = { ...rtspPlayable.value }
  try {
    const res = await datasourceApi.probeRtspUrls(urls)
    const items = res.data.data
    for (const it of items) {
      nextMap[normalizeRtspUrl(it.url)] = it.playable
    }
    rtspPlayable.value = nextMap
    saveRtspProbeCacheToStorage(rtspPlayable.value)
    const okCount = items.filter((i) => i.playable).length
    ElMessage.success(
      `探测完成：${okCount}/${items.length} 路在服务器上通过 ${RTSP_DECODE_PROBE_SECONDS} 秒解码验证，结果已保存`
    )
  } catch (e: unknown) {
    if (axios.isAxiosError(e) && e.code === 'ECONNABORTED') {
      ElMessage.error(
        '请求超时：探测耗时过长。请减少同时探测的路数，或检查服务器到 RTSP 的网络、以及服务器是否已安装 ffmpeg 与 timeout 命令'
      )
    } else {
      const msg = e instanceof Error ? e.message : '探测失败'
      ElMessage.error(msg)
    }
  } finally {
    loading.close()
    rtspProbing.value = false
  }
}

onMounted(() => {
  store.fetchList()
  loadAllFromStorage()
})
</script>

<style scoped>
.page-container {
  padding: 20px;
  max-width: 1200px;
}

.page-header {
  margin-bottom: 16px;
}

.page-header h2 {
  font-size: 20px;
  color: #303133;
}

.rtsp-collapse {
  background: #fff;
  border-radius: 8px;
  padding: 8px 16px;
  border: 1px solid var(--el-border-color-lighter);
}

.collapse-body {
  padding-bottom: 8px;
}

.lead {
  font-size: 14px;
  color: #606266;
  margin-bottom: 12px;
  line-height: 1.5;
}

.rtsp-textarea-large :deep(textarea) {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  font-size: 14px;
  line-height: 1.45;
}

.toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-start;
  gap: 12px;
  margin-top: 12px;
}

.toolbar-left {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  flex-shrink: 0;
}

.form-hint {
  flex: 1;
  min-width: 200px;
  color: #909399;
  font-size: 12px;
  line-height: 1.5;
}

.url-status-list {
  margin-top: 20px;
  padding: 12px;
  background: #f5f7fa;
  border-radius: 6px;
  border: 1px solid var(--el-border-color-lighter);
}

.url-status-head {
  font-size: 13px;
  color: #606266;
  margin-bottom: 10px;
  font-weight: 500;
}

.url-status-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 6px 0;
  border-bottom: 1px solid #ebeef5;
}

.url-status-row:last-child {
  border-bottom: none;
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
  font-size: 13px;
  word-break: break-all;
}

.help-list {
  margin: 0;
  padding-left: 20px;
  color: #606266;
  font-size: 13px;
  line-height: 1.7;
}
</style>
